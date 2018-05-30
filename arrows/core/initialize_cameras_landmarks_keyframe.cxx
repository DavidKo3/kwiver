/*ckwg +29
 * Copyright 2018 by Kitware, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither name of Kitware, Inc. nor the names of any contributors may be used
 *    to endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 * \brief Implementation of keyframe camera and landmark initialization algorithm
 */

#include "initialize_cameras_landmarks_keyframe.h"

#include <random>
#include <unordered_map>
#include <deque>
#include <iterator>
#include <Eigen/StdVector>
#include <fstream>
#include <ctime>

#include <vital/exceptions.h>
#include <vital/io/eigen_io.h>

#include <vital/algo/estimate_essential_matrix.h>
#include <vital/algo/triangulate_landmarks.h>
#include <vital/algo/bundle_adjust.h>
#include <vital/algo/optimize_cameras.h>

#include <arrows/core/triangulate_landmarks.h>
#include <arrows/core/epipolar_geometry.h>
#include <arrows/core/metrics.h>
#include <arrows/core/match_matrix.h>
#include <arrows/core/triangulate.h>
#include <arrows/core/transform.h>
#include <vital/algo/estimate_pnp.h>
#include <vital/types/camera_perspective_map.h>
#include <arrows/core/sfm_utils.h>

#define M_PI 3.141592653589793238462643383279

using namespace kwiver::vital;

namespace kwiver {
namespace arrows {
namespace core {

typedef std::map< frame_id_t, simple_camera_perspective_sptr >               map_cam_t;
typedef std::map<frame_id_t, simple_camera_perspective_sptr>::iterator       cam_map_itr_t;
typedef std::map<frame_id_t, simple_camera_perspective_sptr>::const_iterator const_cam_map_itr_t;

typedef vital::landmark_map::map_landmark_t map_landmark_t;

typedef std::pair<frame_id_t, float> coverage_pair;
typedef std::vector<coverage_pair> frame_coverage_vec;

class rel_pose {
public:
  vital::frame_id_t f0;
  vital::frame_id_t f1;
  matrix_3x3d R01;
  vector_3d t01;
  int well_conditioned_landmark_count;
  double angle_sum;
  float coverage_0;
  float coverage_1;
  static const double target_angle;

  double score(int min_matches = 30) const
  {
    if (well_conditioned_landmark_count == 0)
    {
      return 0;
    }

    double ave_angle = angle_sum / (double(well_conditioned_landmark_count));
    double angle_score = std::max(1.0 - fabs((ave_angle / target_angle) - 1.0), 0.0);

    //having more than 50 features really doesn't improve things
    double count_score = std::min(1.0, double(well_conditioned_landmark_count / 50.0));
    if (well_conditioned_landmark_count < min_matches)
    {
      count_score = 0;
    }

    double coverage_score = std::min(0.2f, std::min(coverage_0, coverage_1));  // more than 20% overlap doesn't help
    return count_score * angle_score * angle_score * coverage_score;
  }

  bool operator<(const rel_pose & other) const
  {
    if (f0 < other.f0)
    {
      return true;
    }
    else if (f0 == other.f0)
    {
      return f1 < other.f1;
    }
    else
    {
      return false;
    }
  }

  friend std::ostream& operator<<(std::ostream& s, rel_pose const& rp);
  friend std::istream& operator>>(std::istream& s, rel_pose & rp);
};

const double rel_pose::target_angle = 20.0 * M_PI / 180.0;

/// output stream operator for a landmark base class
std::ostream&
operator<<(std::ostream& s, rel_pose const& rp)
{
  s << rp.f0 << " "
    << rp.f1 << "\n"
    << rp.R01 << "\n"
    << rp.t01 << "\n"
    << rp.well_conditioned_landmark_count << " "
    << rp.angle_sum << " "
    << rp.coverage_0 << " "
    << rp.coverage_1;
  return s;
}

/// input stream operator for a landmark
std::istream&
operator >> (std::istream& s, rel_pose & rp)
{
  s >> rp.f0
    >> rp.f1
    >> rp.R01
    >> rp.t01
    >> rp.well_conditioned_landmark_count
    >> rp.angle_sum
    >> rp.coverage_0
    >> rp.coverage_1;
  return s;
}

/// Private implementation class
class initialize_cameras_landmarks_keyframe::priv
{
public:
  /// Constructor
  priv();

  /// Destructor
  ~priv();

  /// Pass through this callback to another callback but cache the return value
  bool pass_through_callback(callback_t cb,
                             camera_map_sptr cams,
                             landmark_map_sptr lms);

  void check_inputs(feature_track_set_sptr tracks);

  bool initialize_keyframes(
    simple_camera_perspective_map_sptr cameras,
    landmark_map_sptr& landmarks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata,
    callback_t m_callback);

  bool initialize_remaining_cameras(
    simple_camera_perspective_map_sptr cams,
    landmark_map_sptr& landmarks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata,
    callback_t callback);

  bool bundle_adjust();

  std::set<vital::frame_id_t> get_keyframe_ids(feature_track_set_sptr tracks);

  rel_pose
  calc_rel_pose(frame_id_t frame_0, frame_id_t frame_1,
    const std::vector<track_sptr>& trks) const;

  bool write_rel_poses(std::string file_path) const;

  bool read_rel_poses(std::string file_path);

  void
    calc_rel_poses(
      const std::set<frame_id_t> &frames,
      feature_track_set_sptr tracks);

  /// Re-triangulate all landmarks for provided tracks
  void retriangulate(landmark_map::map_landmark_t& lms,
    simple_camera_perspective_map_sptr cams,
    const std::vector<track_sptr>& trks,
    std::set<landmark_id_t>& inlier_lm_ids) const;

  void triangulate_landmarks_visible_in_frames(
    landmark_map::map_landmark_t& lmks,
    simple_camera_perspective_map_sptr cams,
    feature_track_set_sptr tracks,
    std::set<frame_id_t> frame_ids,
    bool triangulate_only_outliers);

  void down_select_landmarks(
    landmark_map::map_landmark_t &lmks,
    simple_camera_perspective_map_sptr cams,
    feature_track_set_sptr tracks,
    std::set<frame_id_t> down_select_these_frames) const;

  bool initialize_reconstruction(
    simple_camera_perspective_map_sptr cams,
    map_landmark_t &lms,
    feature_track_set_sptr tracks);

  frame_id_t select_next_camera(
    std::set<frame_id_t> &frames_to_resection,
    simple_camera_perspective_map_sptr cams,
    map_landmark_t lms,
    feature_track_set_sptr tracks);

  bool resection_camera(
    simple_camera_perspective_map_sptr cams,
    map_landmark_t lms,
    feature_track_set_sptr tracks,
    frame_id_t fid_to_resection);

  void three_point_pose(frame_id_t frame,
    vital::simple_camera_perspective_sptr &cam,
    feature_track_set_sptr tracks,
    vital::landmark_map::map_landmark_t lms,
    float coverage_threshold,
    vital::algo::estimate_pnp_sptr pnp);

  float image_coverage(
    simple_camera_perspective_map_sptr cams,
    const std::vector<track_sptr>& trks,
    const kwiver::vital::landmark_map::map_landmark_t& lms,
    frame_id_t frame) const;

  void remove_redundant_keyframe(
    simple_camera_perspective_map_sptr cameras,
    landmark_map_sptr& landmarks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata,
    frame_id_t target_frame);

  void remove_redundant_keyframes(
    simple_camera_perspective_map_sptr cameras,
    landmark_map_sptr& landmarks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata,
    std::deque<frame_id_t> &recently_added_frame_queue);

  int get_inlier_count(
    frame_id_t fid,
    landmark_map_sptr landmarks,
    feature_track_set_sptr tracks);

  int set_inlier_flags(
    frame_id_t fid,
    simple_camera_perspective_sptr cam,
    const map_landmark_t &lms,
    feature_track_set_sptr tracks,
    double reporj_thresh);

  void cleanup_necker_reversals(
    simple_camera_perspective_map_sptr cams,
    landmark_map_sptr landmarks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata);

  std::set<landmark_id_t> find_visible_landmarks_in_frames(
    const map_landmark_t &lmks,
    feature_track_set_sptr tracks,
    const std::set<frame_id_t> &frames);

  vector_3d get_velocity(
    simple_camera_perspective_map_sptr cams,
    frame_id_t vel_frame) const;

  void get_registered_and_non_registered_frames(
    simple_camera_perspective_map_sptr cams,
    feature_track_set_sptr tracks,
    std::set<frame_id_t> &registered_frames,
    std::set<frame_id_t> &non_registered_frames) const;

  bool get_next_fid_to_register_and_its_closets_registered_cam(
    simple_camera_perspective_map_sptr cams,
    std::set<frame_id_t> &frames_to_register,
    frame_id_t &fid_to_register, frame_id_t &closest_frame) const;

  landmark_map_sptr get_sub_landmark_map(
    map_landmark_t &lmks,
    const std::set<landmark_id_t> &lm_ids) const;

  landmark_map_sptr store_landmarks(
    map_landmark_t &store_lms,
    landmark_map_sptr to_store) const;

  bool initialize_next_camera(
    simple_camera_perspective_map_sptr cams,
    map_landmark_t& lmks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata,
    frame_id_t &fid_to_register,
    std::set<frame_id_t> &frames_to_register,
    std::set<frame_id_t> &already_registred_cams);

  void windowed_clean_and_bundle(
    simple_camera_perspective_map_sptr cams,
    landmark_map_sptr& landmarks,
    map_landmark_t& lmks,
    feature_track_set_sptr tracks,
    metadata_map_sptr metadata,
    const std::set<frame_id_t> &already_registered_cams,
    const std::set<frame_id_t> &frames_since_last_local_ba);

  bool verbose;
  bool continue_processing;
  double interim_reproj_thresh;
  double final_reproj_thresh;
  double image_coverage_threshold;
  double zoom_scale_thresh;
  vital::simple_camera_perspective m_base_camera;
  vital::algo::estimate_essential_matrix_sptr e_estimator;
  vital::algo::optimize_cameras_sptr camera_optimizer;
  vital::algo::triangulate_landmarks_sptr lm_triangulator;
  vital::algo::bundle_adjust_sptr bundle_adjuster;
  /// Logger handle
  vital::logger_handle_t m_logger;
  double m_thresh_triang_cos_ang;
  vital::algo::estimate_pnp_sptr m_pnp;
  std::set<rel_pose> m_rel_poses;
  Eigen::SparseMatrix<unsigned int> m_kf_match_matrix;
  std::vector<frame_id_t> m_kf_mm_frames;
  std::set<frame_id_t> m_frames_removed_from_sfm_solution;
  vital::track_map_t m_track_map;
  std::random_device m_rd;     // only used once to initialise (seed) engine
  std::mt19937 m_rng;    // random-number engine used (Mersenne-Twister in this case)
  double m_reverse_ba_error_ratio;
  bool m_enable_BA_callback;
};

initialize_cameras_landmarks_keyframe::priv
::priv()
  : verbose(false),
  continue_processing(true),
  interim_reproj_thresh(10.0),
  final_reproj_thresh(2.0),
  image_coverage_threshold(0.05),
  zoom_scale_thresh(0.1),
  m_base_camera(),
  e_estimator(),
  camera_optimizer(),
  // use the core triangulation as the default, users can change it
  lm_triangulator(new core::triangulate_landmarks()),
  bundle_adjuster(),
  m_logger(vital::get_logger("arrows.core.initialize_cameras_landmarks_keyframe")),
  m_thresh_triang_cos_ang(cos((M_PI / 180.0) * 2.0)),
  m_rng(m_rd()),
  m_reverse_ba_error_ratio(2.0),
  m_enable_BA_callback(false)
{

  }

initialize_cameras_landmarks_keyframe::priv
::~priv()
{
}

landmark_map_sptr
initialize_cameras_landmarks_keyframe::priv
::get_sub_landmark_map(map_landmark_t &lmks, const std::set<landmark_id_t> &lm_ids) const
{
  map_landmark_t sub_lmks;
  for (auto lm : lm_ids)
  {
    auto it = lmks.find(lm);
    if (it == lmks.end())
    {
      continue;
    }
    sub_lmks[lm] = it->second;
  }

  return landmark_map_sptr(new simple_landmark_map(sub_lmks));
}

landmark_map_sptr
initialize_cameras_landmarks_keyframe::priv
::store_landmarks(map_landmark_t &store_lms, landmark_map_sptr to_store) const
{
  for (auto lm : to_store->landmarks())
  {
    store_lms[lm.first] = lm.second;
  }
  return landmark_map_sptr(new simple_landmark_map(store_lms));
}

vector_3d
initialize_cameras_landmarks_keyframe::priv
::get_velocity(
  simple_camera_perspective_map_sptr cams,
  frame_id_t vel_frame) const
{
  vector_3d vel;
  vel.setZero();

  auto existing_cams = cams->simple_perspective_cameras();
  auto closest_fid = -1;
  auto next_closest_fid = -1;
  auto it = existing_cams.find(vel_frame);
  simple_camera_perspective_sptr closest_cam, next_closest_cam;
  closest_cam = nullptr;
  next_closest_cam = nullptr;
  frame_id_t min_frame_diff = std::numeric_limits<frame_id_t>::max();

  if (it == existing_cams.end())
  {
    //find the closest existing cam to vel_frame
    for (auto &ec : existing_cams)
    {
      auto diff = abs(ec.first - vel_frame);
      if (diff < min_frame_diff)
      {
        min_frame_diff = diff;
        closest_cam = ec.second;
        closest_fid = ec.first;
      }
    }
  }
  else
  {
    closest_cam = it->second;
  }

  if (!closest_cam)
  {
    return vel;
  }

  // find the second closest cam
  min_frame_diff = std::numeric_limits<frame_id_t>::max();
  for (auto &ec : existing_cams)
  {
    if (ec.first == closest_fid)
    {
      continue;
    }
    auto diff = abs(ec.first - closest_fid);
    if (diff < min_frame_diff)
    {
      min_frame_diff = diff;
      next_closest_cam = ec.second;
      next_closest_fid = ec.first;
    }
  }

  if (!next_closest_cam || min_frame_diff > 4)
  {
    return vel;
  }

  double frame_diff = static_cast<double>(closest_fid - next_closest_fid);
  auto pos_diff = closest_cam->center() - next_closest_cam->center();

  vel = pos_diff * (1.0 / frame_diff);

  return vel;
}

void
initialize_cameras_landmarks_keyframe::priv
::check_inputs(feature_track_set_sptr tracks)
{
  if (!tracks)
  {
    throw invalid_value("required feature tracks are NULL.");
  }
  if (!e_estimator)
  {
    throw invalid_value("Essential matrix estimator not initialized.");
  }
  if (!lm_triangulator)
  {
    throw invalid_value("Landmark triangulator not initialized.");
  }
}


/// Pass through this callback to another callback but cache the return value
bool
initialize_cameras_landmarks_keyframe::priv
::pass_through_callback(callback_t cb,
  camera_map_sptr cams,
  landmark_map_sptr lms)
{
  this->continue_processing = cb(cams, lms);
  return this->continue_processing;
}

std::set<vital::frame_id_t>
initialize_cameras_landmarks_keyframe::priv
::get_keyframe_ids(feature_track_set_sptr tracks)
{
  auto all_frame_ids = tracks->all_frame_ids();
  std::set<vital::frame_id_t> keyframe_ids;
  for (auto fid : all_frame_ids)
  {
    auto fd = std::dynamic_pointer_cast<vital::feature_track_set_frame_data>(tracks->frame_data(fid));
    if (!fd || !fd->is_keyframe)
    {
      continue;
    }
    keyframe_ids.insert(fid);
  }
  return keyframe_ids;
}

/// Re-triangulate all landmarks for provided tracks
void
initialize_cameras_landmarks_keyframe::priv
::retriangulate(landmark_map::map_landmark_t& lms,
  simple_camera_perspective_map_sptr cams,
  const std::vector<track_sptr>& trks,
  std::set<landmark_id_t>& inlier_lm_ids) const
{
  typedef landmark_map::map_landmark_t lm_map_t;
  lm_map_t init_lms;

  for (const track_sptr& t : trks)
  {
    const track_id_t& tid = t->id();
    lm_map_t::const_iterator li = lms.find(tid);
    if (li == lms.end())
    {
      auto lm = std::make_shared<landmark_d>(vector_3d(0, 0, 0));
      init_lms[static_cast<landmark_id_t>(tid)] = lm;
    }
    else
    {
      init_lms.insert(*li);
    }
  }

  landmark_map_sptr lm_map = std::make_shared<simple_landmark_map>(init_lms);
  auto tracks = std::make_shared<feature_track_set>(trks);
  this->lm_triangulator->triangulate(cams, tracks, lm_map);

  inlier_lm_ids.clear();
  lms.clear();
  auto inlier_lms = lm_map->landmarks();
  for(auto lm: inlier_lms)
  {
    inlier_lm_ids.insert(lm.first);
    lms[lm.first] = lm.second;
  }
}

void
initialize_cameras_landmarks_keyframe::priv
::triangulate_landmarks_visible_in_frames(
  landmark_map::map_landmark_t& lmks,
  simple_camera_perspective_map_sptr cams,
  feature_track_set_sptr tracks,
  std::set<frame_id_t> frame_ids,
  bool triangulate_only_outliers)
{
  // only triangulates tracks that don't already have associated landmarks
  landmark_map::map_landmark_t new_lms;
  std::vector<track_sptr> triang_tracks;

  for (auto fid : frame_ids)
  {
    auto active_tracks = tracks->active_tracks(fid);
    for (auto &t : active_tracks)
    {
      auto fts = std::static_pointer_cast<feature_track_state>(*(t->find(fid)));
      if (fts->inlier && triangulate_only_outliers)
      {
        continue;
      }

      auto it = lmks.find(t->id());
      if (it != lmks.end())
      {
        continue;
      }

      triang_tracks.push_back(t);
    }
  }
  std::set<landmark_id_t> inlier_lm_ids;
  retriangulate(new_lms, cams, triang_tracks, inlier_lm_ids);

  for (auto &lm : new_lms)
  {
    lmks[lm.first] = lm.second;
  }
}

class gridded_mask {
public:
  gridded_mask(
    int input_w,
    int input_h,
    int min_features_per_cell):
    m_input_w(input_w),
    m_input_h(input_h),
    m_min_features_per_cell(min_features_per_cell)
  {
    m_mask.setZero();
  }

  void add_entry(vector_2d loc)
  {
    int cx = m_mask.cols()* (loc.x() / double(m_input_w));
    int cy = m_mask.rows()* (loc.y() / double(m_input_h));
    cx = std::min<int>(cx, m_mask.cols()-1);
    cy = std::min<int>(cy, m_mask.rows()-1);
    auto &mv = m_mask(cy, cx);
    ++mv;
  }

  bool conditionally_remove_entry(vector_2d loc)
  {
    int cx = m_mask.cols()* (loc.x() / double(m_input_w));
    int cy = m_mask.rows()* (loc.y() / double(m_input_h));
    cx = std::min<int>(cx, m_mask.cols()-1);
    cy = std::min<int>(cy, m_mask.rows()-1);
    auto &mv = m_mask(cy, cx);
    if (mv > m_min_features_per_cell)
    {
      --mv;
      return true;
    }
    else
    {
      return false;
    }
  }

private:

  int m_input_w;
  int m_input_h;
  int m_min_features_per_cell;
  Eigen::Matrix<int, 4, 4> m_mask;

};

typedef std::shared_ptr<gridded_mask> gridded_mask_sptr;


void
initialize_cameras_landmarks_keyframe::priv
::down_select_landmarks(
  landmark_map::map_landmark_t &lmks,
  simple_camera_perspective_map_sptr cams,
  feature_track_set_sptr tracks,
  std::set<frame_id_t> down_select_these_frames) const
{
  //go through landmarks visible in the down select frames
  // favor longer landmarks
  // get at least N landmarks per image region, if possible
  const int cells_w = 4;
  const int cells_h = 4;
  const int min_features_per_cell = 128 / (cells_w*cells_h);
  std::map<frame_id_t, gridded_mask_sptr> masks;

  frame_id_t first_frame = std::numeric_limits<frame_id_t>::max();
  frame_id_t last_frame = -1;
  std::set<track_sptr> lm_to_downsample_set;
  for (auto ds : down_select_these_frames)
  {
    auto active_tracks = tracks->active_tracks(ds);
    for (auto t : active_tracks)
    {
      if (lmks.find(t->id()) == lmks.end())
      {
        continue;
      }
      first_frame = std::min<frame_id_t>(first_frame, t->first_frame());
      last_frame = std::max<frame_id_t>(last_frame, t->last_frame());
      lm_to_downsample_set.insert(t);
    }
  }

  //ok now we know what frames the down select tracks cover.  Build a mask for each of these frames.
  for (auto cam : cams->simple_perspective_cameras())
  {
    auto fid = cam.first;
    if (fid < first_frame || fid > last_frame)
    {
      continue;
    }
    int image_w = 2 * cam.second->intrinsics()->principal_point().x();
    int image_h = 2 * cam.second->intrinsics()->principal_point().y();

    auto mask = std::make_shared<gridded_mask>(image_w, image_h, min_features_per_cell);
    masks[fid] = mask;

    auto active_tracks = tracks->active_tracks(fid);
    for (auto t : active_tracks)
    {
      if (lmks.find(t->id()) == lmks.end())
      {
        continue;
      }
      auto ts = *t->find(fid);
      auto fts = std::static_pointer_cast<feature_track_state>(ts);
      if (!fts->inlier)
      {
        continue;
      }
      mask->add_entry(fts->feature->loc());
    }
  }

  std::vector<track_sptr> lm_to_downsample;
  for (auto t : lm_to_downsample_set)
  {
    lm_to_downsample.push_back(t);
  }

  std::random_device rd;  //Will be used to obtain a seed for the random number engine
  std::mt19937 gen(rd()); //Standard mersenne_twister_engine seeded with rd()
  const int good_enough_size = 10;
  std::uniform_int_distribution<> dis(2, good_enough_size);

  std::set<landmark_id_t> lm_to_remove;
  //first we sample from the long_ds_landmarks because they will make the reconstruction complete without as many breaks
  while (!lm_to_downsample.empty())
  {
    auto &t1 = lm_to_downsample.back();
    for(int ns = 0; ns < 20; ++ns)
    {
      int rand_idx = std::min<int>((double(std::rand()) / double(RAND_MAX)) * lm_to_downsample.size(), lm_to_downsample.size() - 1);
      t1 = lm_to_downsample[rand_idx];
      int length_thresh = dis(gen);
      int t1_effective_len = std::min<int>(t1->size(), good_enough_size);

      if (t1_effective_len <= length_thresh)
      {
        break;
      }
    }
    auto &t2 = lm_to_downsample.back();
    std::swap(t1, t2);
    auto t = t2;
    lm_to_downsample.pop_back();
    bool keep_lm = false;
    for (auto &ts : *t)
    {
      auto fid = ts->frame();
      auto mask_it = masks.find(fid);
      if (mask_it == masks.end())
      {
        continue;
      }
      auto fts = std::static_pointer_cast<feature_track_state>(ts);
      if (!fts->inlier)
      {
        continue;
      }

      if (!mask_it->second->conditionally_remove_entry(fts->feature->loc()))
      {
        keep_lm = true;
      }
    }
    if (!keep_lm)
    {
      lm_to_remove.insert(t->id());
    }
  }

  for (auto lm_id : lm_to_remove)
  {
    lmks.erase(lm_id);
  }
}

rel_pose
initialize_cameras_landmarks_keyframe::priv
::calc_rel_pose(frame_id_t frame_0, frame_id_t frame_1,
  const std::vector<track_sptr>& trks) const
{
  typedef landmark_map::map_landmark_t lm_map_t;
  // extract coresponding image points and landmarks
  std::vector<vector_2d> pts_right, pts_left;
  for (unsigned int i = 0; i<trks.size(); ++i)
  {
    auto frame_data_0 = std::dynamic_pointer_cast<feature_track_state>(
      *(trks[i]->find(frame_0)));
    auto frame_data_1 = std::dynamic_pointer_cast<feature_track_state>(
      *(trks[i]->find(frame_1)));
    if (!frame_data_0 || !frame_data_1)
    {
      continue;
    }
    pts_right.push_back(frame_data_1->feature->loc());
    pts_left.push_back(frame_data_0->feature->loc());
  }

  const camera_intrinsics_sptr cal_left = m_base_camera.get_intrinsics();
  const camera_intrinsics_sptr cal_right = m_base_camera.get_intrinsics();

  std::vector<bool> inliers;
  essential_matrix_sptr E_sptr = e_estimator->estimate(pts_right, pts_left,
    cal_right, cal_left,
    inliers, interim_reproj_thresh);
  const essential_matrix_d E(*E_sptr);

  unsigned num_inliers = static_cast<unsigned>(std::count(inliers.begin(),
    inliers.end(), true));

  LOG_INFO(m_logger, "E matrix num inliers = " << num_inliers
    << "/" << inliers.size());

  // get the first inlier index
  unsigned int inlier_idx = 0;
  for (; inlier_idx < inliers.size() && !inliers[inlier_idx]; ++inlier_idx);

  // get the first inlier correspondence to
  // disambiguate essential matrix solutions
  vector_2d left_pt = cal_left->unmap(pts_left[inlier_idx]);
  vector_2d right_pt = cal_right->unmap(pts_right[inlier_idx]);

  // compute the corresponding camera rotation and translation (up to scale)
  vital::simple_camera_perspective cam = kwiver::arrows::extract_valid_left_camera(E, left_pt, right_pt);
  cam.set_intrinsics(m_base_camera.intrinsics());

  map_landmark_t lms;

  simple_camera_perspective_map::map_simple_camera_perspective_t cams;
  cams[frame_1] = std::static_pointer_cast<simple_camera_perspective>(cam.clone());
  cams[frame_0] = std::static_pointer_cast<simple_camera_perspective>(m_base_camera.clone());

  auto cam_map = std::make_shared < simple_camera_perspective_map>(cams);
  auto sc_map = std::static_pointer_cast<camera_map>(cam_map);

  auto trk_set = std::make_shared<feature_track_set>(trks);

  std::set<frame_id_t> inlier_lm_ids;
  retriangulate(lms, cam_map, trks, inlier_lm_ids);
  int inlier_count_prev = 0;

  //optimizing loop
  while (inlier_lm_ids.size() > inlier_count_prev)
  {
    inlier_count_prev = inlier_lm_ids.size();
    landmark_map_sptr ba_lms(new simple_landmark_map(lms));
#pragma omp critical
    {
      bundle_adjuster->optimize(sc_map, ba_lms, trk_set);
    }
    inlier_lm_ids.clear();
    retriangulate(lms, cam_map, trks, inlier_lm_ids);
  }

  rel_pose rp;
  rp.f0 = frame_0;
  rp.f1 = frame_1;
  rp.R01 = cam.get_rotation().matrix();
  rp.t01 = cam.translation();
  rp.well_conditioned_landmark_count = inlier_lm_ids.size();
  rp.angle_sum = 0;

  int im_w = 2.0*cams[frame_0]->intrinsics()->principal_point().x();
  int im_h = 2.0*cams[frame_0]->intrinsics()->principal_point().y();
  rp.coverage_0 = image_coverage(cam_map, trks, lms, frame_0);

  im_w = 2.0*cams[frame_1]->intrinsics()->principal_point().x();
  im_h = 2.0*cams[frame_1]->intrinsics()->principal_point().y();
  rp.coverage_1 = image_coverage(cam_map, trks, lms, frame_1);

  for (auto lm : lms)
  {
    double ang = acos(lm.second->cos_obs_angle());
    rp.angle_sum += ang;
  }
  return rp;
}

bool
initialize_cameras_landmarks_keyframe::priv
::write_rel_poses(std::string file_path) const
{
  std::ofstream pose_stream;
  pose_stream.open(file_path);
  if (!pose_stream.is_open())
  {
    return false;
  }

  bool first = true;
  for (auto const& rp : m_rel_poses)
  {
    if (!first)
    {
      pose_stream << "\n";
    }
    pose_stream << rp;
    first = false;
  }

  return true;
}

bool
initialize_cameras_landmarks_keyframe::priv
::read_rel_poses(std::string file_path)
{
  m_rel_poses.clear();
  std::ifstream pose_stream;
  pose_stream.open(file_path);
  if (!pose_stream.is_open())
  {
    return false;
  }

  rel_pose rp;
  while (!pose_stream.eof())
  {
    pose_stream >> rp;
    m_rel_poses.insert(rp);
  }

  return true;
}

void
initialize_cameras_landmarks_keyframe::priv
::calc_rel_poses(
  const std::set<frame_id_t> &frames,
  feature_track_set_sptr tracks)
{
  std::string rel_pose_path = "rel_poses.txt";

  if (read_rel_poses(rel_pose_path))
  {
    return;
  }

  m_kf_mm_frames = std::vector<frame_id_t>(frames.begin(), frames.end());
  m_kf_match_matrix = match_matrix(tracks, m_kf_mm_frames);

  typedef Eigen::Matrix<unsigned int, Eigen::Dynamic, 1> vectorXu;
  const int cols = m_kf_match_matrix.cols();

  const int min_matches = 100;

  std::vector<std::pair<frame_id_t, frame_id_t>> pairs_to_process;
  for (int k = 0; k < cols; ++k)
  {
    for (Eigen::SparseMatrix<unsigned int>::InnerIterator it(m_kf_match_matrix, k); it; ++it)
    {
      if (it.row() > k && it.value() > min_matches)
      {
        auto fid_0 = m_kf_mm_frames[it.row()];
        auto fid_1 = m_kf_mm_frames[k];
        if (fid_0 > fid_1)
        {
          std::swap(fid_0, fid_1);
        }
        pairs_to_process.push_back(std::pair<frame_id_t, frame_id_t>(fid_0, fid_1));
      }
    }
  }

  #pragma omp parallel for schedule(dynamic, 10)
  for (int64_t i = 0; i < pairs_to_process.size(); ++i)
  {
    const auto &tp = pairs_to_process[i];
    auto fid_0 = tp.first;
    auto fid_1 = tp.second;
    auto tks0 = tracks->active_tracks(fid_0);
    auto tks1 = tracks->active_tracks(fid_1);
    std::sort(tks0.begin(), tks0.end());
    std::sort(tks1.begin(), tks1.end());
    std::vector<kwiver::vital::track_sptr> tks_01;
    auto tk_it = std::set_intersection(tks0.begin(), tks0.end(), tks1.begin(), tks1.end(), std::back_inserter(tks_01));

    //ok now we have the common tracks between the two frames.
    //make the essential matrix, decompose it and store it in a relative pose
    rel_pose rp = calc_rel_pose(fid_0, fid_1, tks_01);
    #pragma omp critical
    {
      if (rp.well_conditioned_landmark_count > 0)
      {
        m_rel_poses.insert(rp);
      }
    }
  }

  write_rel_poses(rel_pose_path);
}

frame_id_t
initialize_cameras_landmarks_keyframe::priv
::select_next_camera(
  std::set<frame_id_t> &frames_to_resection,
  simple_camera_perspective_map_sptr cams,
  map_landmark_t lms,
  feature_track_set_sptr tracks)
{
  std::map<frame_id_t, double> resection_frames_score;

  for (const auto& rp : m_rel_poses)
  {
    frame_id_t crossing_resection_frame_id = -1;

    if (cams->find(rp.f0) &&
      frames_to_resection.find(rp.f1) != frames_to_resection.end())
    {
      crossing_resection_frame_id = rp.f1;
    }

    if (cams->find(rp.f1) &&
      frames_to_resection.find(rp.f0) != frames_to_resection.end())
    {
      crossing_resection_frame_id = rp.f0;
    }

    if (crossing_resection_frame_id == -1)
    {
      continue;
    }
    //rel pose spans the current cameras and the frames to resection
    auto rfs_it = resection_frames_score.find(crossing_resection_frame_id);
    double rp_score = rp.score(0);
    if(rfs_it != resection_frames_score.end())
    {

      if (rp_score > rfs_it->second)
      {
        rfs_it->second = rp_score;
      }
    }
    else
    {
      resection_frames_score[crossing_resection_frame_id] = rp_score;
    }
  }

  //find the maximum resection_frames_score and return that pose
  double max_score = -1;
  frame_id_t selected_frame = -1;
  for (auto rfs : resection_frames_score)
  {
    if (rfs.second > max_score)
    {
      selected_frame = rfs.first;
      max_score = rfs.second;
    }
  }
  return selected_frame;
}

/// Calculate fraction of image that is covered by landmark projections
/**
* For the frame find landmarks that project into the frame.  Mark the
* associated feature projection locations as occupied in a mask.  After masks
* has been accumulated calculate the fraction of each mask that is occupied.
* Return this coverage fraction.
* \param [in] tracks the set of feature tracks
* \param [in] lms landmarks to check coverage on
* \param [in] frame the frame to have coverage calculated on
* \param [in] im_w width of images
* \param [in] in_h height of images
* \return     the coverage fraction range [0 - 1]
*/

float
initialize_cameras_landmarks_keyframe::priv
::image_coverage(
  simple_camera_perspective_map_sptr cams,
  const std::vector<track_sptr>& trks,
  const kwiver::vital::landmark_map::map_landmark_t& lms,
  frame_id_t frame) const
{
  vital::camera_map::map_camera_t cam_map;
  cam_map[frame] = cams->find(frame);
  std::set<frame_id_t> frames;
  frames.insert(frame);
  auto coverages = image_coverages(trks, lms, cam_map);
  return coverages[0].second;
}


void
initialize_cameras_landmarks_keyframe::priv
::three_point_pose(frame_id_t frame,
  vital::simple_camera_perspective_sptr &cam,
  feature_track_set_sptr tracks,
  vital::landmark_map::map_landmark_t lms,
  float coverage_threshold,
  vital::algo::estimate_pnp_sptr pnp)
{
  std::vector<vital::vector_2d> pts2d;
  std::vector<vital::vector_3d> pts3d;
  kwiver::vital::camera_intrinsics_sptr cal;
  std::vector<bool> inliers;
  typedef std::pair<landmark_id_t, landmark_sptr> lm_pair;
  std::vector<lm_pair> frame_landmarks;
  std::vector<feature_track_state_sptr> frame_feats;
  auto tks = tracks->active_tracks(frame);
  for (auto tk : tks)
  {
    vital::track_id_t tid = tk->id();
    auto ts = tk->find(frame);
    if (ts == tk->end())
    {
      continue;  //just a double check but this should not happen
    }

    feature_track_state_sptr fts = std::dynamic_pointer_cast<feature_track_state>(*ts);
    if (!fts || !fts->feature)
    {
      continue;  //make sure it's a feature track state.  Always should be.
    }

    auto lm_it = lms.find(tid);
    if (lm_it == lms.end())
    {
      continue;  //no landmark for this track
    }

    //ok we have a landmark with an associated track.
    pts3d.push_back(lm_it->second->loc());
    pts2d.push_back(fts->feature->loc());
    frame_feats.push_back(fts);
    frame_landmarks.push_back(lm_pair(lm_it->first, lm_it->second));
  }
  cam = std::static_pointer_cast<simple_camera_perspective>(pnp->estimate(pts2d, pts3d, cam->intrinsics(), inliers));

  size_t num_inliers = 0;
  float coverage = 0;
  if (cam)
  {
    std::map<landmark_id_t, landmark_sptr> inlier_lms;

    for (size_t i = 0; i < inliers.size(); ++i)
    {
      if (inliers[i])
      {
        ++num_inliers;
        inlier_lms.insert(frame_landmarks[i]);
        frame_feats[i]->inlier = true;
      }
      else
      {
        frame_feats[i]->inlier = false;
      }
    }
    auto cams = std::make_shared<simple_camera_perspective_map>();
    cams->insert(frame, cam);
    coverage = image_coverage(cams, tks, inlier_lms, frame);
  }

  LOG_DEBUG(m_logger, "for frame " << frame << " P3P found " << num_inliers <<
    " inliers out of " << inliers.size() <<
    " feature projections with coverage " << coverage);

  if (coverage < coverage_threshold)
  {
    LOG_DEBUG(m_logger, "resectioning image " << frame <<
      " failed: insufficient coverage ( " << coverage << " )");
    cam = simple_camera_perspective_sptr();
  }
}

bool
initialize_cameras_landmarks_keyframe::priv
::resection_camera(
  simple_camera_perspective_map_sptr cams,
  map_landmark_t lms,
  feature_track_set_sptr tracks,
  frame_id_t fid_to_resection)
{
  LOG_DEBUG(m_logger, "resectioning camera " << fid_to_resection);
  std::shared_ptr<vital::simple_camera_perspective> nc(std::static_pointer_cast<simple_camera_perspective>(m_base_camera.clone()));
  nc->set_intrinsics(m_base_camera.intrinsics());

  //do 3PT algorithm here
  three_point_pose(fid_to_resection, nc, tracks, lms,
    image_coverage_threshold, m_pnp);

  if (!nc)
  {
    //three point pose failed.
    m_frames_removed_from_sfm_solution.insert(fid_to_resection);
    return false;
  }
  else
  {
    cams->insert(fid_to_resection, nc);

    camera_map_sptr nr_cams(new simple_camera_map(cams->cameras()));
    landmark_map_sptr nr_lms(new simple_landmark_map(lms));

    //ret reprojecion error for this camera
    float non_rev_rmse =
      kwiver::arrows::reprojection_rmse(nr_cams->cameras(),
        nr_lms->landmarks(), tracks->tracks());

    //get reprojection error of it's necker reversed camera
    necker_reverse(nr_cams, nr_lms, false);

    float rev_rmse =
      kwiver::arrows::reprojection_rmse(nr_cams->cameras(),
        nr_lms->landmarks(), tracks->tracks());

    LOG_DEBUG(m_logger, "P3P init RMSE " << non_rev_rmse << " reveresed RMSE " << rev_rmse);

    // if necker reversed camera is lower then switch to it
    if (rev_rmse < non_rev_rmse)
    {
      LOG_DEBUG(m_logger, "Switching to necker reversed camera for frame " << fid_to_resection);
      simple_camera_perspective_sptr reversed_cam = std::static_pointer_cast<simple_camera_perspective>(nr_cams->cameras()[fid_to_resection]);
      cams->insert(fid_to_resection, reversed_cam);
    }
    return true;
  }
}

bool
initialize_cameras_landmarks_keyframe::priv
::initialize_reconstruction(
  simple_camera_perspective_map_sptr cams,
  map_landmark_t &lms,
  feature_track_set_sptr tracks)
{
  struct {
    bool operator()(const rel_pose &a, const rel_pose &b) const
    {
      return a.score() < b.score();
    }
  } well_conditioned_less;
  std::vector<rel_pose> rel_poses_inlier_ordered(m_rel_poses.begin(), m_rel_poses.end());
  std::sort(rel_poses_inlier_ordered.begin(), rel_poses_inlier_ordered.end(), well_conditioned_less);
  std::reverse(rel_poses_inlier_ordered.begin(), rel_poses_inlier_ordered.end());

  bool good_initialization = false;
  for (auto &rp_init : rel_poses_inlier_ordered)
  {
    cams->clear();
    lms.clear();
    auto cam_0 = std::make_shared<vital::simple_camera_perspective>();
    auto cam_1 = std::make_shared<vital::simple_camera_perspective>();
    cam_0->set_intrinsics(m_base_camera.intrinsics());
    cam_1->set_intrinsics(m_base_camera.intrinsics());
    cam_1->set_rotation(vital::rotation_d(rp_init.R01));
    cam_1->set_translation(rp_init.t01);
    cams->insert(rp_init.f0, cam_0);
    cams->insert(rp_init.f1, cam_1);

    auto trks = tracks->tracks();

    std::set<landmark_id_t> inlier_lm_ids;
    retriangulate(lms, cams, trks, inlier_lm_ids);

    LOG_DEBUG(m_logger, "rp_init.well_conditioned_landmark_count " << rp_init.well_conditioned_landmark_count << " inlier_lm_ids size " << inlier_lm_ids.size());

    if (bundle_adjuster)
    {
      LOG_INFO(m_logger, "Running Global Bundle Adjustment on "
        << cams->size() << " cameras and "
        << lms.size() << " landmarks");

      auto cam_map = cams->cameras();
      double before_clean_rmse = kwiver::arrows::reprojection_rmse(cam_map, lms, trks);
      LOG_INFO(m_logger, "before clean reprojection RMSE: " << before_clean_rmse);

      camera_map_sptr ba_cams(new simple_camera_map(cam_map));
      landmark_map_sptr ba_lms(new simple_landmark_map(lms));
      double init_rmse = kwiver::arrows::reprojection_rmse(ba_cams->cameras(), lms, trks);
      LOG_INFO(m_logger, "initial reprojection RMSE: " << init_rmse);

      //TODO:  Change bundle adjuster so it takes a simple_camera_perspective_map_sptr as input to avoid all this copying.
      bundle_adjuster->optimize(ba_cams, ba_lms, tracks);

      //set cams from ba_cams
      cams->set_from_base_cams(ba_cams);

      lms = ba_lms->landmarks();
      auto first_cam = std::static_pointer_cast<simple_camera_perspective>(ba_cams->cameras().begin()->second);
      m_base_camera.set_intrinsics(first_cam->intrinsics());

      double optimized_rmse = kwiver::arrows::reprojection_rmse(cams->cameras(), lms, trks);
      LOG_INFO(m_logger, "optimized reprojection RMSE: " << optimized_rmse);

      inlier_lm_ids.clear();
      retriangulate(lms, cams, trks, inlier_lm_ids);

      LOG_DEBUG(m_logger, "after ba rp_init.well_conditioned_landmark_count " << rp_init.well_conditioned_landmark_count << " inlier_lm_ids size " << inlier_lm_ids.size());

      std::vector<frame_id_t> removed_cams;
      std::set<frame_id_t> variable_cams;
      std::set<landmark_id_t> variable_lms;
      cam_map = cams->cameras();
      clean_cameras_and_landmarks(cam_map, lms, tracks, m_thresh_triang_cos_ang, removed_cams, variable_cams, variable_lms, image_coverage_threshold, interim_reproj_thresh);

      bool try_next_pair = false;
      for (auto c : cam_map)
      {
        if (!c.second)
        {
          try_next_pair = true;
          break;
        }
      }
      if (try_next_pair)
      {
        continue;
      }


    }
    if (lms.size() > 0.1 * rp_init.well_conditioned_landmark_count)
    {
      LOG_DEBUG(m_logger, "initialization pair is  " << rp_init.f0 << ", " << rp_init.f1);
      good_initialization = true;
      break;
    }
  }
  return good_initialization;
}

template<class T>
class set_map {
public:
  set_map() {};
  ~set_map() {};
  void add_set(const std::set<T> &in_set)
  {
    size_t hash = hash_set(in_set);
    if (!contains_set(in_set, hash))
    {
      m_map.insert(std::pair<size_t,std::set<T>>(hash, in_set));
    }
  }

  bool remove_set(const std::set<T> &rem_set)
  {
    size_t hash = hash_set(test_set);
    auto it_pair = m_map.equal_range(hash);
    for (auto it = it_pair.first; it != it_pair.second; ++it)
    {
      std::set<T> &potential_match_set = *it;
      if (rem_set == potential_match_set)
      {
        m_map.erase(it);
        return true;
      }
    }
    return false;
  }

  bool contains_set(const std::set<T> &test_set)
  {
    size_t hash = hash_set(test_set);
    return contains_set(test_set, hash);
  }

  bool contains_set(const std::set<T> &test_set, size_t hash)
  {
    auto it_pair = m_map.equal_range(hash);
    for (auto it = it_pair.first; it != it_pair.second; ++it)
    {
      std::set<T> &potential_match_set = it->second;
      if (test_set == potential_match_set)
      {
        return true;
      }
    }
    return false;
  }

private:
  size_t hash_set(const std::set<T> &to_hash)
  {
    size_t sum = 0;
    for (auto & val : to_hash)
    {
      sum += val;
    }
    return sum;
  }
  std::unordered_multimap<size_t,std::set<T>> m_map;
};

void initialize_cameras_landmarks_keyframe::priv
::remove_redundant_keyframe(
  simple_camera_perspective_map_sptr cams,
  landmark_map_sptr& landmarks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata,
  frame_id_t target_frame)
{

  if (m_track_map.empty())
  {
    //only build this once.
    auto tks = tracks->tracks();
    for (auto const& t : tks)
    {
      m_track_map[t->id()] = t;
    }
  }

  auto lmks = landmarks->landmarks();
  //build up camera to landmark and landmark to camera maps
  int landmarks_involving_target_frame = 0;
  int landmarks_lost_without_target_frame = 0;

  auto c = cams->find(target_frame);

  if (!c || c->center().x() == 0 && c->center().y() == 0 && c->center().z() == 0)
  {
    return;
  }

  for (auto const& lm : lmks)
  {
    auto t = lm.first;
    bool found_target_frame = false;
    int lm_inlier_meas = 0;

    auto tk_it = m_track_map.find(t);
    if (tk_it == m_track_map.end())
    {
      continue;
    }
    auto tk = tk_it->second;
    for (auto ts_it = tk->begin(); ts_it != tk->end(); ++ts_it)
    {
      auto fts = static_cast<feature_track_state*>(ts_it->get());
      if (!fts->inlier)
      {
        continue;
      }

      auto const f = fts->frame();

      if (!cams->find(f))
      {
        continue;
      }
      //landmark is an inlier to one of the cameras in the reconstruction
      if (f == target_frame)
      {
        found_target_frame = true;
      }
      ++lm_inlier_meas;
    }

    if (found_target_frame)
    {
      ++landmarks_involving_target_frame;
      if (lm_inlier_meas <= 3)
      {
        ++landmarks_lost_without_target_frame;
      }
    }
  }

  if (landmarks_lost_without_target_frame < 0.1 * landmarks_involving_target_frame)
  {
    //less than 5% of the landmarks involving the target frame would have less than three masurements
    // if the target frame was removed.  So we will remove it.
    cams->erase(target_frame);

    std::set<landmark_id_t> inlier_lms;
    retriangulate(lmks, cams, tracks->tracks(), inlier_lms);

    landmarks = landmark_map_sptr(new simple_landmark_map(lmks));
  }
}

void initialize_cameras_landmarks_keyframe::priv
::remove_redundant_keyframes(
  simple_camera_perspective_map_sptr cams,
  landmark_map_sptr& landmarks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata,
  std::deque<frame_id_t> &recently_added_frame_queue)
{
  if (recently_added_frame_queue.size() >= 5)
  {
    std::set<frame_id_t> latest_frames;
    for (auto it = recently_added_frame_queue.begin(); it != recently_added_frame_queue.end(); ++it)
    {
      latest_frames.insert(*it);
    }
    recently_added_frame_queue.pop_front();

    for (int rem_try = 0; rem_try < 10; ++rem_try)
    {
      auto cameras = cams->cameras();
      std::uniform_int_distribution<size_t> uni(0, cameras.size() - 1); // guaranteed unbiased
      auto ri = uni(m_rng);
      auto cams_it = cameras.begin();
      for (int i = 0; i < ri; ++i)
      {
        ++cams_it;
      }
      frame_id_t potential_rem_frame = cams_it->first;
      if (latest_frames.find(potential_rem_frame) != latest_frames.end())
      {
        continue;
      }
      remove_redundant_keyframe(cams, landmarks, tracks, metadata, potential_rem_frame);
    }
  }
}

bool
initialize_cameras_landmarks_keyframe::priv
::initialize_keyframes(
  simple_camera_perspective_map_sptr cams,
  landmark_map_sptr& landmarks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata,
  callback_t callback)
{
  LOG_DEBUG(m_logger, "initialize_keyframes");


  // get set of keyframe ids
  auto keyframes = get_keyframe_ids(tracks);
  if (keyframes.empty())
  {
    LOG_DEBUG(m_logger, "no keyframes, cannot initilize reconstruction");
    return false;
  }

  //const int num_begining_keyframes(std::min<int>(keyframes.size(),20));
  auto first_fid = *keyframes.begin();
  auto ff_tracks = tracks->active_tracks(first_fid);
  auto all_frames = tracks->all_frame_ids();

  std::vector<frame_id_t> last_continuous_track_frames;
  for (auto t : ff_tracks)
  {
    int tl = 0;
    frame_id_t lf = -1;
    for (auto fid : all_frames)
    {
      if (t->find(fid) != t->end())
      {
        ++tl;
        lf = fid;
      }
      else
      {
        break;
      }
    }
    //exclude very short tracks
    if (lf != -1 && tl > 2)
    {
      last_continuous_track_frames.push_back(lf);
    }
  }

  if (last_continuous_track_frames.empty())
  {
    return false;
  }
  std::sort(last_continuous_track_frames.begin(), last_continuous_track_frames.end());

  auto last_kf_for_init = last_continuous_track_frames[last_continuous_track_frames.size()*0.70];

  LOG_DEBUG(m_logger, "last_kf_for_init " << last_kf_for_init);

  std::set<frame_id_t> beginning_keyframes;
  for (auto kf_id : keyframes)
  {
    if (kf_id > last_kf_for_init)
    {
      break;
    }
    beginning_keyframes.insert(kf_id);
    if (beginning_keyframes.size() >= 40)
    {
      break;
    }
  }

  map_landmark_t lms;

  LOG_DEBUG(m_logger, "beginning_keyframes size " << beginning_keyframes.size());

  // get relative pose constraints for keyframes
  calc_rel_poses(beginning_keyframes, tracks);

  // To do 1, visual odometry

  // choose the first keyframe pair that meets minimum triangulation angle requirements.  This is the seed.
  // Triangulate first pair
  initialize_reconstruction(cams, lms, tracks);

  if (callback)
  {
    continue_processing =
      callback(cams,std::make_shared<simple_landmark_map>(lms));

    if (!continue_processing)
    {
      LOG_DEBUG(m_logger,
        "continue processing is false, exiting initialize loop");
      landmarks = landmark_map_sptr(new simple_landmark_map(lms));

      return true;
    }
  }

  //list remaining frames to resection
  std::set<frame_id_t> frames_to_resection = keyframes;
  auto sc_map = cams->simple_perspective_cameras();
  for (auto c : sc_map)
  {
    frames_to_resection.erase(c.first);
  }

  //landmark_map_sptr join_lms(new simple_landmark_map(lms));
  //join_landmarks(cams, join_lms, tracks, metadata);
  //lms = join_lms->landmarks();

  bool tried_necker_reverse = false;
  int prev_ba_lm_count = lms.size();
  auto trks = tracks->tracks();

  int frames_resectioned_since_last_ba = 0;
  std::deque<frame_id_t> added_frame_queue;
  while (!frames_to_resection.empty() && (cams->size() < 10 || tried_necker_reverse == false))
  {
    frame_id_t next_frame_id = select_next_camera(frames_to_resection, cams, lms, tracks);

    if (!resection_camera(cams, lms, tracks, next_frame_id))
    {
      frames_to_resection.erase(next_frame_id);
      continue;
    }

    ++frames_resectioned_since_last_ba;

    added_frame_queue.push_back(next_frame_id);

    // we just resectioned this camera so we can try all the previously
    // failed frames again.
    for (auto ffid : m_frames_removed_from_sfm_solution)
    {
      frames_to_resection.insert(ffid);
    }
    //remove all the failed to resection images.  We will try them again.
    m_frames_removed_from_sfm_solution.clear();

    frames_to_resection.erase(next_frame_id);

    {
      //bundle adjust fixing all cameras but the new one
      auto cameras = cams->cameras();
      camera_map_sptr ba_cams(new simple_camera_map(cams->cameras()));
      landmark_map_sptr ba_lms(new simple_landmark_map(lms));

      double before_new_cam_rmse = kwiver::arrows::reprojection_rmse(cameras, lms, trks);
      LOG_INFO(m_logger, "before new camera reprojection RMSE: " << before_new_cam_rmse);

      std::set<frame_id_t> fixed_cameras;
      std::set<landmark_id_t> fixed_landmarks;
      for (auto c : cameras)
      {
        if (c.first != next_frame_id)
        {
          fixed_cameras.insert(c.first);
        }
      }

      bundle_adjuster->optimize(ba_cams, ba_lms, tracks, fixed_cameras, fixed_landmarks, metadata);

      lms = ba_lms->landmarks();
      cams->set_from_base_cams(ba_cams);
      auto first_cam = std::static_pointer_cast<simple_camera_perspective>(ba_cams->cameras().begin()->second);
      m_base_camera.set_intrinsics(first_cam->intrinsics());

      double after_new_cam_rmse = kwiver::arrows::reprojection_rmse(cams->cameras(), lms, trks);
      LOG_INFO(m_logger, "after new camera reprojection RMSE: " << after_new_cam_rmse);

    }
    std::set<landmark_id_t> inlier_lm_ids;
    retriangulate(lms, cams, trks, inlier_lm_ids);

    //landmark_map_sptr join_lms(new simple_landmark_map(lms));
    //join_landmarks(cams, join_lms, tracks, metadata, next_frame_id);

    //remove_redundant_keyframes(cams, join_lms, tracks, metadata, added_frame_queue);

    //lms = join_lms->landmarks();
    {
      std::vector<frame_id_t> removed_cams;
      auto cam_map = cams->cameras();
      std::set<frame_id_t> variable_cams;
      std::set<landmark_id_t> variable_lms;
      clean_cameras_and_landmarks(cam_map, lms, tracks, m_thresh_triang_cos_ang, removed_cams, variable_cams, variable_lms, image_coverage_threshold, interim_reproj_thresh);

      for (auto rem_fid : removed_cams)
      {
        m_frames_removed_from_sfm_solution.insert(rem_fid);
      }
      cams->set_from_base_camera_map(cam_map);
    }

    int next_ba_cam_count = std::max<int>(cams->size() * 0.2,5);

    if ( (lms.size() > prev_ba_lm_count * 1.5 ||
      lms.size() < prev_ba_lm_count * 0.5) ||
      frames_resectioned_since_last_ba >= next_ba_cam_count ||
      frames_to_resection.empty())
    {
      frames_resectioned_since_last_ba = 0;
      //bundle adjust result because number of inliers has changed significantly
      if ( bundle_adjuster)
      {
        LOG_INFO(m_logger, "Running Global Bundle Adjustment on "
          << cams->size() << " cameras and "
          << lms.size() << " landmarks");

        auto cam_map = cams->cameras();
        double before_clean_rmse = kwiver::arrows::reprojection_rmse(cam_map, lms, trks);
        LOG_INFO(m_logger, "before clean reprojection RMSE: " << before_clean_rmse);

        std::vector<frame_id_t> removed_cams;
        std::set<frame_id_t> variable_cams;
        std::set<landmark_id_t> variable_lms;
        clean_cameras_and_landmarks(cam_map, lms, tracks, m_thresh_triang_cos_ang, removed_cams, variable_cams, variable_lms, image_coverage_threshold, interim_reproj_thresh);
        for (auto rem_fid : removed_cams)
        {
          m_frames_removed_from_sfm_solution.insert(rem_fid);
        }
        camera_map_sptr ba_cams(new simple_camera_map(cam_map));
        landmark_map_sptr ba_lms(new simple_landmark_map(lms));

        double init_rmse = kwiver::arrows::reprojection_rmse(cam_map, lms, trks);
        LOG_INFO(m_logger, "initial reprojection RMSE: " << init_rmse);

        //first a BA fixing all landmarks to correct the cameras
        std::set<frame_id_t> fixed_cameras;
        std::set<landmark_id_t> fixed_landmarks;
        for (auto l : lms)
        {
          fixed_landmarks.insert(l.first);
        }
        bundle_adjuster->optimize(ba_cams, ba_lms, tracks, fixed_cameras, fixed_landmarks, metadata);

        lms = ba_lms->landmarks();
        cams->set_from_base_cams(ba_cams);
        auto first_cam = std::static_pointer_cast<simple_camera_perspective>(ba_cams->cameras().begin()->second);
        m_base_camera.set_intrinsics(first_cam->intrinsics());

        double optimized_rmse = kwiver::arrows::reprojection_rmse(ba_cams->cameras(), lms, trks);
        LOG_INFO(m_logger, "optimized reprojection RMSE: " << optimized_rmse);

        retriangulate(lms, cams, trks, inlier_lm_ids);

        //now an overall ba
        landmark_map_sptr ba_lms2(new simple_landmark_map(lms));
        fixed_landmarks.clear();
        bundle_adjuster->optimize(ba_cams, ba_lms2, tracks, fixed_cameras, fixed_landmarks, metadata);

        cams->set_from_base_cams(ba_cams);
        lms = ba_lms2->landmarks();
        first_cam = std::static_pointer_cast<simple_camera_perspective>(ba_cams->cameras().begin()->second);
        m_base_camera.set_intrinsics(first_cam->intrinsics());

        //do an overall join, because the solution geometry may have changed significantly after BA.

        //landmark_map_sptr join_lms(new simple_landmark_map(lms));
        //join_landmarks(cams, join_lms, tracks, metadata);
        //lms = join_lms->landmarks();

        if (!tried_necker_reverse && m_reverse_ba_error_ratio > 0)
        {
          // reverse cameras and optimize again

          camera_map_sptr ba_cams2(new simple_camera_map(cams->cameras()));
          landmark_map_sptr ba_lms2(new simple_landmark_map(lms));
          necker_reverse(ba_cams2, ba_lms2);
          lm_triangulator->triangulate(ba_cams2, tracks, ba_lms2);
          init_rmse = kwiver::arrows::reprojection_rmse(ba_cams2->cameras(), ba_lms2->landmarks(), trks);
          LOG_DEBUG(m_logger, "Necker reversed initial reprojection RMSE: " << init_rmse);
          if (init_rmse < optimized_rmse * m_reverse_ba_error_ratio)
          {
            // Only try a Necker reversal once when we have enough data to
            // support it. We will either decide to reverse or not.
            // Either way we should not have to try this again.
            tried_necker_reverse = true;
            LOG_INFO(m_logger, "Running Necker reversed bundle adjustment for comparison");
            bundle_adjuster->optimize(ba_cams2, ba_lms2, tracks, metadata);

            map_landmark_t lms2 = ba_lms2->landmarks();

            double final_rmse2 = kwiver::arrows::reprojection_rmse(ba_cams2->cameras(), lms2, trks);
            LOG_DEBUG(m_logger, "Necker reversed final reprojection RMSE: " << final_rmse2);

            if (final_rmse2 < optimized_rmse)
            {
              LOG_INFO(m_logger, "Necker reversed solution is better");
              cams->set_from_base_cams(ba_cams2);
              lms = ba_lms2->landmarks();
            }
          }
        }

        prev_ba_lm_count = lms.size();

        if (!continue_processing)
        {
          break;
        }
      }
    }
    if (callback)
    {
      continue_processing =
      callback(cams,std::make_shared<simple_landmark_map>(lms));

      if (!continue_processing)
      {
        LOG_DEBUG(m_logger,
          "continue processing is false, exiting initialize loop");
        break;
      }
    }
  }

  auto cam_map = cams->cameras();
  std::vector<frame_id_t> removed_cams;
  std::set<frame_id_t> empty_cam_set;
  std::set<landmark_id_t> empty_lm_set;
  clean_cameras_and_landmarks(cam_map, lms, tracks, m_thresh_triang_cos_ang, removed_cams, empty_cam_set, empty_lm_set, image_coverage_threshold, interim_reproj_thresh);

  cams->set_from_base_camera_map(cam_map);
  landmarks = landmark_map_sptr(new simple_landmark_map(lms));

  return true;
}

int
initialize_cameras_landmarks_keyframe::priv
::get_inlier_count(frame_id_t fid, landmark_map_sptr landmarks, feature_track_set_sptr tracks)
{
  int inlier_count = 0;
  auto lmks = landmarks->landmarks();
  auto cur_tracks = tracks->active_tracks(fid);
  for (auto &t : cur_tracks)
  {
    auto ts = t->find(fid);
    if (ts == t->end())
    {
      continue;
    }

    auto fts = std::dynamic_pointer_cast<feature_track_state>(*ts);
    if (!fts || !fts->feature)
    {
      continue;
    }

    if (lmks.find(t->id()) == lmks.end())
    {
      continue;
    }
    if (fts->inlier)
    {
      ++inlier_count;
    }
  }
  return inlier_count;
}

int initialize_cameras_landmarks_keyframe::priv
::set_inlier_flags(
    frame_id_t fid,
    simple_camera_perspective_sptr cam,
    const map_landmark_t &lms,
    feature_track_set_sptr tracks,
    double reporj_thresh)
{
  const double reporj_thresh_sq = reporj_thresh*reporj_thresh;
  int inlier_count = 0;
  auto vital_cam = std::static_pointer_cast<camera>(cam);
  auto cur_tracks = tracks->active_tracks(fid);
  for (auto &t : cur_tracks)
  {
    auto ts = t->find(fid);
    if (ts == t->end())
    {
      continue;
    }

    auto fts = std::dynamic_pointer_cast<feature_track_state>(*ts);
    if (!fts || !fts->feature)
    {
      continue;
    }

    auto lm_it = lms.find(t->id());

    if (lm_it == lms.end())
    {
      continue;
    }

    double err = reprojection_error_sqr(*vital_cam, *lm_it->second, *fts->feature);
    if (err < reporj_thresh_sq)
    {
      fts->inlier = true;
      ++inlier_count;
    }
    else
    {
      fts->inlier = false;
    }
  }
  return inlier_count;
}

void
initialize_cameras_landmarks_keyframe::priv
::cleanup_necker_reversals(
  simple_camera_perspective_map_sptr cams,
  landmark_map_sptr landmarks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata)
{
  //first record all camera positions
  std::map<frame_id_t, vector_3d> orig_positions;
  auto spc = cams->simple_perspective_cameras();
  std::set<frame_id_t> fixed_cams;
  for (auto &c : spc)
  {
    orig_positions[c.first] = c.second->center();
    fixed_cams.insert(c.first);
  }

  auto lms = landmarks->landmarks();
  std::set<landmark_id_t> fixed_landmarks;
  for (auto const&lm : lms)
  {
    fixed_landmarks.insert(lm.first);
  }

  simple_camera_perspective_sptr prev_cam;
  for (auto &cur_cam_pair : spc)
  {
    auto fid = cur_cam_pair.first;
    if (prev_cam)
    {
      auto cur_cam = cur_cam_pair.second;
      auto nr_cam = std::make_shared<simple_camera_perspective_map>();
      nr_cam->insert(fid, std::static_pointer_cast<simple_camera_perspective>(cur_cam->clone()));
      camera_map_sptr nr_cam_base(new simple_camera_map(nr_cam->cameras()));
      necker_reverse(nr_cam_base, landmarks, false);
      auto reversed_cam = std::static_pointer_cast<simple_camera_perspective>(nr_cam_base->cameras().begin()->second);

      auto non_rev_dist = (cur_cam->center() - prev_cam->center()).norm();
      auto rev_dist = (reversed_cam->center() - prev_cam->center()).norm();
      if (rev_dist < non_rev_dist)
      {
        //remove current frame from fixed cameras so it will be optimized
        fixed_cams.erase(fid);
        cams->insert(fid, reversed_cam);
        camera_map_sptr ba_cams(new simple_camera_map(cams->cameras()));
        bundle_adjuster->optimize(ba_cams, landmarks, tracks, fixed_cams, fixed_landmarks, metadata);
        cams->set_from_base_cams(ba_cams);
        //now add it back to the fixed frames so it is fixed next time
        fixed_cams.insert(fid);
      }
    }
    //make sure we get the camera from cams in case it was changed by a reversal
    prev_cam = cams->find(fid);
  }

  //ok, now all cams should be consistent with the first cam.  Do I reverse them all
  //first get all the camera pointers again, in case they have changed.
  spc = cams->simple_perspective_cameras();
  int reverse_it_if_positive = 0;
  for (auto cur_cam_pair : spc)
  {
    auto fid = cur_cam_pair.first;
    auto cur_cam = cur_cam_pair.second;
    auto nr_cam = std::make_shared<simple_camera_perspective_map>();
    nr_cam->insert(fid, std::static_pointer_cast<simple_camera_perspective>(cur_cam->clone()));
    camera_map_sptr nr_cam_base(new simple_camera_map(nr_cam->cameras()));
    necker_reverse(nr_cam_base, landmarks, false);
    auto reversed_cam = std::static_pointer_cast<simple_camera_perspective>(nr_cam_base->cameras().begin()->second);
    auto orig_center = orig_positions[fid];
    auto non_rev_dist = (cur_cam->center() - orig_center).norm();
    auto rev_dist = (reversed_cam->center() - orig_center).norm();
    if (rev_dist < non_rev_dist)
    {
      ++reverse_it_if_positive;
    }
    else
    {
      --reverse_it_if_positive;
    }
  }

  if (reverse_it_if_positive > 0)
  {
    for (auto cur_cam_pair : spc)
    {
      auto fid = cur_cam_pair.first;
      auto cur_cam = cur_cam_pair.second;
      auto nr_cam = std::make_shared<simple_camera_perspective_map>();
      nr_cam->insert(fid, std::static_pointer_cast<simple_camera_perspective>(cur_cam->clone()));
      camera_map_sptr nr_cam_base(new simple_camera_map(nr_cam->cameras()));
      necker_reverse(nr_cam_base, landmarks, false);
      auto reversed_cam = std::static_pointer_cast<simple_camera_perspective>(nr_cam_base->cameras().begin()->second);
      cams->insert(fid, reversed_cam);
    }
  }

  fixed_cams.clear();
  camera_map_sptr ba_cams(new simple_camera_map(cams->cameras()));
  bundle_adjuster->optimize(ba_cams, landmarks, tracks, fixed_cams, fixed_landmarks, metadata);
  cams->set_from_base_cams(ba_cams);

}

std::set<landmark_id_t>
initialize_cameras_landmarks_keyframe::priv
::find_visible_landmarks_in_frames(
  const map_landmark_t &lmks,
  feature_track_set_sptr tracks,
  const std::set<frame_id_t> &frames)
{
  std::set<landmark_id_t> visible_landmarks;

  for (auto const fid : frames)
  {
    auto at = tracks->active_tracks(fid);
    for (auto t : at)
    {
      //auto t_it = m_track_to_landmark_map.find(t->id());
      //if (t_it != m_track_to_landmark_map.end())
      //{
      //  visible_landmarks.insert(t_it->second);
      //}
      visible_landmarks.insert(t->id());
    }
  }

  return visible_landmarks;
}

void
initialize_cameras_landmarks_keyframe::priv
::get_registered_and_non_registered_frames(
  simple_camera_perspective_map_sptr cams,
  feature_track_set_sptr tracks,
  std::set<frame_id_t> &registered_frames,
  std::set<frame_id_t> &non_registered_frames) const
{
  registered_frames.clear();
  non_registered_frames.clear();

  auto pcams_map = cams->simple_perspective_cameras();
  non_registered_frames = tracks->all_frame_ids();
  for (auto &p : pcams_map)
  {
    registered_frames.insert(p.first);
    non_registered_frames.erase(p.first);
  }
}

bool
initialize_cameras_landmarks_keyframe::priv
::get_next_fid_to_register_and_its_closets_registered_cam(
  simple_camera_perspective_map_sptr cams,
  std::set<frame_id_t> &frames_to_register,
  frame_id_t &fid_to_register, frame_id_t &closest_frame) const
{
  auto existing_cams = cams->simple_perspective_cameras();
  frame_id_t min_frame_diff = std::numeric_limits<frame_id_t>::max();
  for (auto f : frames_to_register)
  {
    for (auto &ec : existing_cams)
    {
      auto diff = abs(ec.first - f);
      if (diff < min_frame_diff)
      {
        closest_frame = ec.first;
        min_frame_diff = diff;
        fid_to_register = f;
      }
    }
  }
  if (min_frame_diff != std::numeric_limits<frame_id_t>::max())
  {
    return true;
  }
  else
  {
    return false;
  }
}

bool
initialize_cameras_landmarks_keyframe::priv
::initialize_next_camera(
  simple_camera_perspective_map_sptr cams,
  map_landmark_t& lmks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata,
  frame_id_t &fid_to_register,
  std::set<frame_id_t> &frames_to_register,
  std::set<frame_id_t> &already_registred_cams)
{
  frame_id_t closest_cam_fid;
  get_next_fid_to_register_and_its_closets_registered_cam(cams, frames_to_register, fid_to_register, closest_cam_fid);
  frames_to_register.erase(fid_to_register);

  simple_camera_perspective_sptr closest_cam = cams->simple_perspective_cameras()[closest_cam_fid];
  simple_camera_perspective_sptr resectioned_cam, bundled_cam;
  int bundled_inlier_count = 0;
  int resection_inlier_count = 0;
  bool good_pose = false;

  int min_inliers = 50;

  std::set<frame_id_t> cur_fid;
  cur_fid.insert(fid_to_register);
  auto cur_frame_landmarks = find_visible_landmarks_in_frames(lmks, tracks, cur_fid);
  auto cur_landmarks = get_sub_landmark_map(lmks, cur_frame_landmarks);

  auto ba_config = bundle_adjuster->get_configuration();
  bool opt_focal_was_set = ba_config->get_value<bool>("optimize_focal_length");
  ba_config->set_value<bool>("optimize_focal_length", false);
  bundle_adjuster->set_configuration(ba_config);

  {
    auto vel = get_velocity(cams, fid_to_register);
    //use the pose of the closest camera as starting point
    bundled_cam = std::static_pointer_cast<simple_camera_perspective>(closest_cam->clone());
    //use constant velocity model
    bundled_cam->set_center(closest_cam->center() + (fid_to_register - closest_cam_fid) * vel);
    cams->insert(fid_to_register, bundled_cam);
    camera_map_sptr ba_cams(new simple_camera_map(cams->cameras()));

    int prev_bundled_inlier_count = -1;
    int loop_count = 0;
    while (bundled_inlier_count > prev_bundled_inlier_count)
    {
      //DOES THIS LOOP HELP?
      bundle_adjuster->optimize(ba_cams, cur_landmarks, tracks, already_registred_cams, cur_frame_landmarks, metadata);

      cams->set_from_base_cams(ba_cams);
      bundled_cam = cams->find(fid_to_register);
      prev_bundled_inlier_count = bundled_inlier_count;
      bundled_inlier_count = set_inlier_flags(fid_to_register, bundled_cam, cur_landmarks->landmarks(), tracks, 50);
      ++loop_count;
    }
    if (loop_count > 2)
    {
      LOG_DEBUG(m_logger, "ran " << loop_count << " hill climbing BA loops");
    }
  }

  if (bundled_inlier_count < 4 * min_inliers)
  {
    resection_camera(cams, lmks, tracks, fid_to_register);
    resectioned_cam = cams->find(fid_to_register);
    if (resectioned_cam)
    {
      camera_map_sptr ba_cams(new simple_camera_map(cams->cameras()));
      int prev_resection_inlier_count = -1;
      int loop_count = 0;
      while (resection_inlier_count > prev_resection_inlier_count)
      {
        //DOES THIS LOOP HELP?
        bundle_adjuster->optimize(ba_cams, cur_landmarks, tracks, already_registred_cams, cur_frame_landmarks, metadata);
        cams->set_from_base_cams(ba_cams);
        resectioned_cam = cams->find(fid_to_register);
        prev_resection_inlier_count = resection_inlier_count;
        resection_inlier_count = set_inlier_flags(fid_to_register, resectioned_cam, cur_landmarks->landmarks(), tracks, 50);
        ++loop_count;
      }
      if (loop_count > 2)
      {
        LOG_DEBUG(m_logger, "ran " << loop_count << " hill climbing resection BA loops");
      }
    }
  }

  ba_config = bundle_adjuster->get_configuration();
  ba_config->set_value<bool>("optimize_focal_length", opt_focal_was_set);
  bundle_adjuster->set_configuration(ba_config);

  int inlier_count = std::max(resection_inlier_count, bundled_inlier_count);
  if (inlier_count < min_inliers)
  {
    cams->erase(fid_to_register);
    return false;
  }

  if (resection_inlier_count > bundled_inlier_count)
  {
    LOG_DEBUG(m_logger, "using resectioned camera for frame " << fid_to_register << " because resection inlier count " << resection_inlier_count << " greater than bundled inlier count " << bundled_inlier_count);
    cams->insert(fid_to_register, resectioned_cam);
    set_inlier_flags(fid_to_register, resectioned_cam, lmks, tracks, 10);
  }
  else
  {
    cams->insert(fid_to_register, bundled_cam);
    set_inlier_flags(fid_to_register, bundled_cam, lmks, tracks, 10);
  }
  return true;
}

void
initialize_cameras_landmarks_keyframe::priv
::windowed_clean_and_bundle(
  simple_camera_perspective_map_sptr cams,
  landmark_map_sptr& landmarks,
  map_landmark_t& lmks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata,
  const std::set<frame_id_t> &already_registered_cams,
  const std::set<frame_id_t> &frames_since_last_local_ba)
{
  auto frames_to_fix = already_registered_cams;
  //optimize camera and all landmarks it sees, fixing all other cameras.
  auto variable_frames = frames_since_last_local_ba;

  for (auto fid : variable_frames)
  {
    frames_to_fix.erase(fid);
  }

  std::set<landmark_id_t> variable_landmark_ids = find_visible_landmarks_in_frames(lmks, tracks, variable_frames);
  std::vector<frame_id_t> removed_cams;
  auto cam_map = cams->cameras();
  clean_cameras_and_landmarks(cam_map, lmks, tracks, m_thresh_triang_cos_ang, removed_cams,
    variable_frames, variable_landmark_ids, image_coverage_threshold, interim_reproj_thresh);

  for (auto rem_fid : removed_cams)
  {
    m_frames_removed_from_sfm_solution.insert(rem_fid);
  }
  cams->set_from_base_camera_map(cam_map);

  auto variable_landmarks = get_sub_landmark_map(lmks, variable_landmark_ids);
  camera_map_sptr ba_cams(new simple_camera_map(cams->cameras()));
  std::set<landmark_id_t> empty_landmark_id_set;

  bundle_adjuster->optimize(ba_cams, variable_landmarks, tracks, frames_to_fix, empty_landmark_id_set, metadata);
  cams->set_from_base_cams(ba_cams);
  landmarks = store_landmarks(lmks, variable_landmarks);
}

bool
initialize_cameras_landmarks_keyframe::priv
::initialize_remaining_cameras(
  simple_camera_perspective_map_sptr cams,
  landmark_map_sptr& landmarks,
  feature_track_set_sptr tracks,
  metadata_map_sptr metadata,
  callback_t callback)
{
  //we will lock the original cameras in the bundle adjustment, could also exclude them
  time_t prev_callback_time;
  time(&prev_callback_time);
  const double callback_min_period = 2;
  int frames_since_last_down_select = 0;

  auto lmks = landmarks->landmarks();

  std::set<frame_id_t> already_registred_cams, frames_to_register;
  get_registered_and_non_registered_frames(cams, tracks, already_registred_cams, frames_to_register);

  // this forces a BA and cleaning of all cams after the first camera insertion
  std::set<frame_id_t> frames_since_last_local_ba = already_registred_cams;
  m_frames_removed_from_sfm_solution.clear();

  std::set<landmark_id_t> last_ba_landmarks;

  while(!frames_to_register.empty())
  {
    frame_id_t fid_to_register;

    if (!initialize_next_camera(cams, lmks, tracks, metadata, fid_to_register, frames_to_register, already_registred_cams))
    {
      continue;
    }

    //Triangulate only landmarks visible in latest cameras
    std::set<frame_id_t> fids_to_triang;
    fids_to_triang.insert(fid_to_register);
    triangulate_landmarks_visible_in_frames(lmks, cams, tracks, fids_to_triang,false);

    if (++frames_since_last_down_select >= 5)
    {
      down_select_landmarks(lmks, cams, tracks, fids_to_triang);
      frames_since_last_down_select = 0;
    }

    frames_since_last_local_ba.insert(fid_to_register);

    auto cur_lmks = find_visible_landmarks_in_frames(lmks, tracks, fids_to_triang);
    std::vector<landmark_id_t> intersect_lmks, union_lmks;
    std::set_intersection(last_ba_landmarks.begin(), last_ba_landmarks.end(), cur_lmks.begin(), cur_lmks.end(), std::back_inserter(intersect_lmks));
    std::set_union(last_ba_landmarks.begin(), last_ba_landmarks.end(), cur_lmks.begin(), cur_lmks.end(), std::back_inserter(union_lmks));
    float i_over_u = static_cast<float>(intersect_lmks.size()) / static_cast<float>(union_lmks.size());

    if (i_over_u < 0.7)
    {
      windowed_clean_and_bundle(cams, landmarks, lmks, tracks,
        metadata, already_registred_cams, frames_since_last_local_ba);
      last_ba_landmarks = cur_lmks;
      frames_since_last_local_ba.clear();
    }

    already_registred_cams.insert(fid_to_register);

    time_t cur_time;
    time(&cur_time);
    double seconds_since_last_disp = difftime(cur_time, prev_callback_time);

    if (callback && seconds_since_last_disp > callback_min_period)
    {
      time(&prev_callback_time);
      continue_processing =
        callback(cams, std::make_shared<simple_landmark_map>(lmks));

      if (!continue_processing)
      {
        LOG_DEBUG(m_logger,
          "continue processing is false, exiting initialize_remaining_cameras loop");
        break;
      }
    }
  }

  //std::set<frame_id_t> variable_frames;
  //std::set<landmark_id_t> variable_landmarks;
  //std::vector<frame_id_t> removed_cams;
  //auto cam_map = cams->cameras();
  //auto lms = landmarks->landmarks();
  //clean_cameras_and_landmarks(cam_map, lms, tracks, m_thresh_triang_cos_ang, removed_cams,
  //  variable_frames, variable_landmarks, image_coverage_threshold, interim_reproj_thresh);

  //cams->set_from_base_camera_map(cam_map);
  //landmarks = landmark_map_sptr(new simple_landmark_map(lms));

  //for (auto rem_fid : removed_cams)
  //{
  //  m_frames_removed_from_sfm_solution.insert(rem_fid);
  //}

  //for (auto c : m_frames_removed_from_sfm_solution)
  //{
  //  cams->erase(c);
  //}

  //this should only be done for aerial, semi-planar scenes.
  //cleanup_necker_reversals(cams, landmarks, tracks,metadata);

  return true;
}

bool
initialize_cameras_landmarks_keyframe::priv
::bundle_adjust()
{
  return true;
}

//-----------------------------------------------------------------------------
// start: initialize_cameras_landmarks_keyframe

/// Constructor
initialize_cameras_landmarks_keyframe
::initialize_cameras_landmarks_keyframe()
: m_priv(new priv)
{
}

/// Destructor
initialize_cameras_landmarks_keyframe
::~initialize_cameras_landmarks_keyframe()
{
}

/// Get this algorithm's \link vital::config_block configuration block \endlink
vital::config_block_sptr
initialize_cameras_landmarks_keyframe
::get_configuration() const
{
  // get base config from base class
  vital::config_block_sptr config =
      vital::algo::initialize_cameras_landmarks::get_configuration();

  const camera_intrinsics_sptr K = m_priv->m_base_camera.get_intrinsics();

  config->set_value("verbose", m_priv->verbose,
                    "If true, write status messages to the terminal showing "
                    "debugging information");

  config->set_value("interim_reproj_thresh", m_priv->interim_reproj_thresh,
                    "Threshold for rejecting landmarks based on reprojection "
                    "error (in pixels) during intermediate processing steps.");

  config->set_value("final_reproj_thresh", m_priv->final_reproj_thresh,
                    "Relative threshold for rejecting landmarks based on "
                    "reprojection error relative to the median error after "
                    "the final bundle adjustment.  For example, a value of 2 "
                    "mean twice the median error");

  config->set_value("zoom_scale_thresh", m_priv->zoom_scale_thresh,
                    "Threshold on image scale change used to detect a camera "
                    "zoom. If the resolution on target changes by more than "
                    "this fraction create a new camera intrinsics model.");

  config->set_value("base_camera:focal_length", K->focal_length(),
                    "focal length of the base camera model");

  config->set_value("base_camera:principal_point", K->principal_point().transpose(),
                    "The principal point of the base camera model \"x y\".\n"
                    "It is usually safe to assume this is the center of the "
                    "image.");

  config->set_value("base_camera:aspect_ratio", K->aspect_ratio(),
                    "the pixel aspect ratio of the base camera model");

  config->set_value("base_camera:skew", K->skew(),
                    "The skew factor of the base camera model.\n"
                    "This is almost always zero in any real camera.");

  // nested algorithm configurations
  vital::algo::estimate_essential_matrix
      ::get_nested_algo_configuration("essential_mat_estimator",
                                      config, m_priv->e_estimator);
  vital::algo::optimize_cameras
      ::get_nested_algo_configuration("camera_optimizer",
                                      config, m_priv->camera_optimizer);
  vital::algo::triangulate_landmarks
      ::get_nested_algo_configuration("lm_triangulator",
                                      config, m_priv->lm_triangulator);
  vital::algo::bundle_adjust
      ::get_nested_algo_configuration("bundle_adjuster",
                                      config, m_priv->bundle_adjuster);
  vital::algo::estimate_pnp
    ::get_nested_algo_configuration("estimate_pnp", config, m_priv->m_pnp);

  return config;
}


/// Set this algorithm's properties via a config block
void
initialize_cameras_landmarks_keyframe
::set_configuration(vital::config_block_sptr config)
{
  const camera_intrinsics_sptr K = m_priv->m_base_camera.get_intrinsics();

  // Set nested algorithm configurations
  vital::algo::estimate_essential_matrix
      ::set_nested_algo_configuration("essential_mat_estimator",
                                      config, m_priv->e_estimator);
  vital::algo::optimize_cameras
      ::set_nested_algo_configuration("camera_optimizer",
                                      config, m_priv->camera_optimizer);
  vital::algo::triangulate_landmarks
      ::set_nested_algo_configuration("lm_triangulator",
                                      config, m_priv->lm_triangulator);
  vital::algo::bundle_adjust
      ::set_nested_algo_configuration("bundle_adjuster",
                                      config, m_priv->bundle_adjuster);
  if(m_priv->bundle_adjuster && this->m_callback && m_priv->m_enable_BA_callback)
  {
    using std::placeholders::_1;
    using std::placeholders::_2;
    callback_t pcb =
      std::bind(&initialize_cameras_landmarks_keyframe::priv::pass_through_callback,
                m_priv.get(), this->m_callback, _1, _2);
    m_priv->bundle_adjuster->set_callback(pcb);
  }

  m_priv->verbose = config->get_value<bool>("verbose",
                                        m_priv->verbose);

  m_priv->interim_reproj_thresh =
      config->get_value<double>("interim_reproj_thresh",
                                m_priv->interim_reproj_thresh);

  m_priv->final_reproj_thresh =
      config->get_value<double>("final_reproj_thresh",
                                m_priv->final_reproj_thresh);

  m_priv->zoom_scale_thresh =
      config->get_value<double>("zoom_scale_thresh",
                                m_priv->zoom_scale_thresh);

  vital::config_block_sptr bc = config->subblock("base_camera");
  simple_camera_intrinsics K2(bc->get_value<double>("focal_length",
                                                    K->focal_length()),
                              bc->get_value<vector_2d>("principal_point",
                                                       K->principal_point()),
                              bc->get_value<double>("aspect_ratio",
                                                    K->aspect_ratio()),
                              bc->get_value<double>("skew",
                                                    K->skew()));
  m_priv->m_base_camera.set_intrinsics(K2.clone());

  vital::algo::estimate_pnp::set_nested_algo_configuration(
    "estimate_pnp", config, m_priv->m_pnp);
}


/// Check that the algorithm's currently configuration is valid
bool
initialize_cameras_landmarks_keyframe
::check_configuration(vital::config_block_sptr config) const
{
  if (config->get_value<std::string>("camera_optimizer", "") != ""
      && !vital::algo::optimize_cameras
              ::check_nested_algo_configuration("camera_optimizer", config))
  {
    return false;
  }
  if (config->get_value<std::string>("bundle_adjuster", "") != ""
      && !vital::algo::bundle_adjust
              ::check_nested_algo_configuration("bundle_adjuster", config))
  {
    return false;
  }
  return vital::algo::estimate_essential_matrix
             ::check_nested_algo_configuration("essential_mat_estimator",
                                               config)
      && vital::algo::triangulate_landmarks
             ::check_nested_algo_configuration("lm_triangulator", config);
}





/// Initialize the camera and landmark parameters given a set of tracks
void
initialize_cameras_landmarks_keyframe
::initialize(camera_map_sptr& cameras,
             landmark_map_sptr& landmarks,
             feature_track_set_sptr tracks,
             metadata_map_sptr metadata) const
{
  m_priv->check_inputs(tracks);

  auto cams = std::make_shared<simple_camera_perspective_map>();
  cams->set_from_base_cams(cameras);

  // build constraint graph
  m_priv->initialize_keyframes(cams, landmarks, tracks, metadata,this->m_callback);

  // optimize constraint graph
  m_priv->initialize_remaining_cameras(cams,landmarks,tracks,metadata,this->m_callback);

  cameras = std::make_shared<simple_camera_map>(cams->cameras());
}


/// Set a callback function to report intermediate progress
void
initialize_cameras_landmarks_keyframe
::set_callback(callback_t cb)
{
  vital::algo::initialize_cameras_landmarks::set_callback(cb);
  // pass callback on to bundle adjuster if available
  if(m_priv->bundle_adjuster && m_priv->m_enable_BA_callback)
  {
    using std::placeholders::_1;
    using std::placeholders::_2;
    callback_t pcb =
      std::bind(&initialize_cameras_landmarks_keyframe::priv::pass_through_callback,
                m_priv.get(), cb, _1, _2);
    m_priv->bundle_adjuster->set_callback(pcb);
  }
}

} // end namespace core
} // end namespace arrows
} // end namespace kwiver
