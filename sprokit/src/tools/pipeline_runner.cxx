/*ckwg +29
 * Copyright 2011-2018 by Kitware, Inc.
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

#include "pipeline_runner.h"
#include "tool_support.h"

#include <vital/config/config_block.h>
#include <vital/plugin_loader/plugin_manager.h>

#include <sprokit/pipeline/scheduler.h>
#include <sprokit/pipeline/scheduler_factory.h>
#include <sprokit/pipeline/pipeline.h>

#include <cstdlib>
#include <iostream>

namespace sprokit {
namespace tools {

static const auto scheduler_block = kwiver::vital::config_block_key_t("_scheduler");


// ----------------------------------------------------------------------------
pipeline_runner::
pipeline_runner()
{
}


// ----------------------------------------------------------------------------
/*
 * It is not optimal to have the usage text separate form where the
 * command line options are added, since that also can generate
 * usage/help output, but the usage text is needed after the applet is
 * instantiated and before it is run. Also, the format generated by
 * the command_args class is not that pretty.
 */
void
pipeline_runner::
usage( std::ostream& outstream ) const
{
  outstream << "This program runs the specified pipeline file.\n"
            << "Usage: " + applet_name() + " pipe-file [options]\n"
            << "\nOptions are:\n"
            << "     --help  | -h                Output help message and quit.\n"
            << "     --config | -c   FILE        File containing supplemental configuration entries.\n"
            << "                                 Can occurr multiple times.\n"
            << "     --setting | -s   VAR=VALUE  Additional configuration entries.\n"
            << "                                 Can occurr multiple times.\n"
            << "     --include | -I   DIR        A directory to be added to configuration include path.\n"
            << "                                 Can occurr multiple times.\n"
            << "     --scheduler | -S   TYPE     Scheduler type to use.\n"
    ;
}


// ----------------------------------------------------------------------------
int
pipeline_runner::
run( const std::vector<std::string>& argv )
{
  tool_support options;

  options.init_args( argv );    // Add common options
  options.add_pipeline_run_options(); // add pipeline runner options

  if ( ! options.process_args() )
  {
    exit( 0 );
  }

  if ( options.opt_help )
  {
    usage( std::cout );
    return EXIT_SUCCESS;
  }

  // Check for required pipeline file
  if( options.remaining_argc <= 1 )
  {
    usage( std::cout );
    return EXIT_FAILURE;
  }

  // Load all known modules
  kwiver::vital::plugin_manager& vpm = kwiver::vital::plugin_manager::instance();
  vpm.load_all_plugins();

  sprokit::pipeline_builder builder;

  // Add search path to builder.
  options.builder.add_search_path( options.opt_search_path );

  // Load the pipeline file.
  kwiver::vital::path_t const pipe_file( options.remaining_argv[1] );
  options.builder.load_pipeline( pipe_file );

  // Must be applied after pipe file is loaded.
  // To overwrite any existing settings
  options.add_options_to_builder();

  // Get handle to pipeline
  sprokit::pipeline_t const pipe = options.builder.pipeline();

  // get handle to config block
  kwiver::vital::config_block_sptr const conf = options.builder.config();

  if (!pipe)
  {
    std::cerr << "Error: Unable to bake pipeline" << std::endl;
    return EXIT_FAILURE;
  }

  pipe->setup_pipeline();

  //
  // Check for scheduler specification in config block
  //
  auto scheduler_type = sprokit::scheduler_factory::default_type;

  // Check if scheduler type was on the command line.
  if ( ! options.opt_scheduler.empty() )
  {
    scheduler_type = options.opt_scheduler;
  }
  else
  {
    scheduler_type = conf->get_value(
        scheduler_block + kwiver::vital::config_block::block_sep + "type",  // key string
        sprokit::scheduler_factory::default_type ); // default value
  }

  // Get scheduler sub block based on selected scheduler type
  kwiver::vital::config_block_sptr const scheduler_config = conf->subblock(scheduler_block +
                                              kwiver::vital::config_block::block_sep + scheduler_type);

  sprokit::scheduler_t scheduler = sprokit::create_scheduler(scheduler_type, pipe, scheduler_config);

  if (!scheduler)
  {
    std::cerr << "Error: Unable to create scheduler" << std::endl;

    return EXIT_FAILURE;
  }

  scheduler->start();
  scheduler->wait();

  return EXIT_SUCCESS;
}

} } // end namespace
