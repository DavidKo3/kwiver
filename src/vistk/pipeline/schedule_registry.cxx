/*ckwg +5
 * Copyright 2011 by Kitware, Inc. All Rights Reserved. Please refer to
 * KITWARE_LICENSE.TXT for licensing information, or contact General Counsel,
 * Kitware, Inc., 28 Corporate Drive, Clifton Park, NY 12065.
 */

#include "schedule_registry.h"
#include "schedule_registry_exception.h"

#include "types.h"

#include <boost/foreach.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/tuple/tuple.hpp>

#include <map>

/**
 * \file schedule_registry.cxx
 *
 * \brief Implementation of the \link vistk::schedule_registry schedule registry\endlink.
 */

namespace vistk
{

schedule_registry::type_t const schedule_registry::default_type = type_t("thread_per_process");

class schedule_registry::priv
{
  public:
    priv();
    ~priv();

    static schedule_registry_t self;

    typedef boost::tuple<description_t, schedule_ctor_t> schedule_typeinfo_t;
    typedef std::map<type_t, schedule_typeinfo_t> schedule_store_t;
    schedule_store_t registry;

    typedef std::set<module_t> loaded_modules_t;
    loaded_modules_t loaded_modules;
};

schedule_registry_t schedule_registry::priv::self = schedule_registry_t();

schedule_registry
::~schedule_registry()
{
}

void
schedule_registry
::register_schedule(type_t const& type, description_t const& desc, schedule_ctor_t ctor)
{
  if (!ctor)
  {
    throw null_schedule_ctor_exception(type);
  }

  if (d->registry.find(type) != d->registry.end())
  {
    throw schedule_type_already_exists_exception(type);
  }

  d->registry[type] = priv::schedule_typeinfo_t(desc, ctor);
}

schedule_t
schedule_registry
::create_schedule(type_t const& type, config_t const& config, pipeline_t const& pipe) const
{
  if (!config)
  {
    throw null_schedule_registry_config_exception();
  }

  if (!pipe)
  {
    throw null_schedule_registry_pipeline_exception();
  }

  priv::schedule_store_t::const_iterator const i = d->registry.find(type);

  if (i == d->registry.end())
  {
    throw no_such_schedule_type_exception(type);
  }

  return i->second.get<1>()(config, pipe);
}

schedule_registry::types_t
schedule_registry
::types() const
{
  types_t ts;

  BOOST_FOREACH (priv::schedule_store_t::value_type const& entry, d->registry)
  {
    ts.push_back(entry.first);
  }

  return ts;
}

schedule_registry::description_t
schedule_registry
::description(type_t const& type) const
{
  priv::schedule_store_t::const_iterator const i = d->registry.find(type);

  if (i == d->registry.end())
  {
    throw no_such_schedule_type_exception(type);
  }

  return i->second.get<0>();
}

schedule_registry_t
schedule_registry
::self()
{
  static boost::mutex mut;

  if (priv::self)
  {
    return priv::self;
  }

  boost::unique_lock<boost::mutex> lock(mut);
  if (!priv::self)
  {
    priv::self = schedule_registry_t(new schedule_registry);
  }

  return priv::self;
}

schedule_registry
::schedule_registry()
{
  d = boost::shared_ptr<priv>(new priv);
}

schedule_registry::priv
::priv()
{
}

schedule_registry::priv
::~priv()
{
}

}
