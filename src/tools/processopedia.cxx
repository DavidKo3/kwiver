/*ckwg +5
 * Copyright 2011-2013 by Kitware, Inc. All Rights Reserved. Please refer to
 * KITWARE_LICENSE.TXT for licensing information, or contact General Counsel,
 * Kitware, Inc., 28 Corporate Drive, Clifton Park, NY 12065.
 */

#include <sprokit/tools/tool_main.h>
#include <sprokit/tools/tool_usage.h>

#include <sprokit/pipeline/config.h>
#include <sprokit/pipeline/modules.h>
#include <sprokit/pipeline/process.h>
#include <sprokit/pipeline/process_registry.h>
#include <sprokit/pipeline/process_registry_exception.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/foreach.hpp>

#include <iostream>
#include <string>

#include <cstdlib>

static std::string const hidden_prefix = "_";

static boost::program_options::options_description processopedia_options();

int
sprokit_tool_main(int argc, char const* argv[])
{
  sprokit::load_known_modules();

  boost::program_options::options_description desc;
  desc
    .add(sprokit::tool_common_options())
    .add(processopedia_options());

  boost::program_options::variables_map const vm = sprokit::tool_parse(argc, argv, desc);

  sprokit::process_registry_t const reg = sprokit::process_registry::self();

  sprokit::process::types_t types;

  if (vm.count("type"))
  {
    types = vm["type"].as<sprokit::process::types_t>();
  }
  else
  {
    types = reg->types();
  }

  if (vm.count("list"))
  {
    BOOST_FOREACH (sprokit::process::type_t const& type, types)
    {
      std::cout << type << std::endl;
    }

    return EXIT_SUCCESS;
  }

  bool const hidden = vm.count("hidden");

  BOOST_FOREACH (sprokit::process::type_t const& proc_type, types)
  {
    try
    {
      if (!vm.count("detail"))
      {
        std::cout << proc_type << ": " << reg->description(proc_type) << std::endl;

        continue;
      }

      std::cout << "Process type: " << proc_type << std::endl;
      std::cout << "  Description: " << reg->description(proc_type) << std::endl;
    }
    catch (sprokit::no_such_process_type_exception const& e)
    {
      std::cerr << "Error: " << e.what() << std::endl;

      continue;
    }

    sprokit::process_t const proc = reg->create_process(proc_type, sprokit::process::name_t());

    sprokit::process::properties_t const properties = proc->properties();
    std::string const properties_str = boost::join(properties, ", ");

    std::cout << "  Properties: " << properties_str << std::endl;

    std::cout << "  Configuration:" << std::endl;

    sprokit::config::keys_t const keys = proc->available_config();

    BOOST_FOREACH (sprokit::config::key_t const& key, keys)
    {
      if (!hidden && boost::starts_with(key, hidden_prefix))
      {
        continue;
      }

      sprokit::process::conf_info_t const info = proc->config_info(key);

      sprokit::config::value_t const& def = info->def;
      sprokit::config::description_t const& conf_desc = info->description;
      bool const& tunable = info->tunable;

      std::cout << "    Name       : " << key << std::endl;
      std::cout << "    Default    : " << def << std::endl;
      std::cout << "    Description: " << conf_desc << std::endl;
      std::cout << "    Tunable    : " << tunable << std::endl;
      std::cout << std::endl;
    }

    std::cout << "  Input ports:" << std::endl;

    sprokit::process::ports_t const iports = proc->input_ports();

    BOOST_FOREACH (sprokit::process::port_t const& port, iports)
    {
      if (!hidden && boost::starts_with(port, hidden_prefix))
      {
        continue;
      }

      sprokit::process::port_info_t const info = proc->input_port_info(port);

      sprokit::process::port_type_t const& type = info->type;
      sprokit::process::port_flags_t const& flags = info->flags;
      sprokit::process::port_description_t const& port_desc = info->description;

      std::string const flags_str = boost::join(flags, ", ");

      std::cout << "    Name       : " << port << std::endl;
      std::cout << "    Type       : " << type << std::endl;
      std::cout << "    Flags      : " << flags_str << std::endl;
      std::cout << "    Description: " << port_desc << std::endl;
      std::cout << std::endl;
    }

    std::cout << "  Output ports:" << std::endl;

    sprokit::process::ports_t const oports = proc->output_ports();

    BOOST_FOREACH (sprokit::process::port_t const& port, oports)
    {
      if (!hidden && boost::starts_with(port, hidden_prefix))
      {
        continue;
      }

      sprokit::process::port_info_t const info = proc->output_port_info(port);

      sprokit::process::port_type_t const& type = info->type;
      sprokit::process::port_flags_t const& flags = info->flags;
      sprokit::process::port_description_t const& port_desc = info->description;

      std::string const flags_str = boost::join(flags, ", ");

      std::cout << "    Name       : " << port << std::endl;
      std::cout << "    Type       : " << type << std::endl;
      std::cout << "    Flags      : " << flags_str << std::endl;
      std::cout << "    Description: " << port_desc << std::endl;
      std::cout << std::endl;
    }

    std::cout << std::endl;
    std::cout << std::endl;
  }

  return EXIT_SUCCESS;
}

boost::program_options::options_description
processopedia_options()
{
  boost::program_options::options_description desc;

  desc.add_options()
    ("type,t", boost::program_options::value<sprokit::process::types_t>()->value_name("TYPE"), "type to describe")
    ("list,l", "simply list types")
    ("hidden,H", "show hidden properties")
    ("detail,d", "output detailed information")
  ;

  return desc;
}
