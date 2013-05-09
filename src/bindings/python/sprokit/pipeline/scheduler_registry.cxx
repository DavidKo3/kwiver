/*ckwg +5
 * Copyright 2011-2012 by Kitware, Inc. All Rights Reserved. Please refer to
 * KITWARE_LICENSE.TXT for licensing information, or contact General Counsel,
 * Kitware, Inc., 28 Corporate Drive, Clifton Park, NY 12065.
 */

#include <sprokit/pipeline/scheduler.h>
#include <sprokit/pipeline/scheduler_registry.h>

#include <sprokit/python/util/python_threading.h>
#include <sprokit/python/util/python_gil.h>

#include <boost/python/suite/indexing/vector_indexing_suite.hpp>
#include <boost/python/class.hpp>
#include <boost/python/module.hpp>
#include <boost/python/object.hpp>
#include <boost/python/wrapper.hpp>

/**
 * \file scheduler_registry.cxx
 *
 * \brief Python bindings for \link sprokit::scheduler_registry\endlink.
 */

using namespace boost::python;

static void register_scheduler(sprokit::scheduler_registry_t reg,
                               sprokit::scheduler_registry::type_t const& type,
                               sprokit::scheduler_registry::description_t const& desc,
                               object obj);

BOOST_PYTHON_MODULE(scheduler_registry)
{
  class_<sprokit::scheduler_registry::type_t>("SchedulerType"
    , "The type for a type of scheduler.");
  class_<sprokit::scheduler_registry::description_t>("SchedulerDescription"
    , "The type for a description of a scheduler type.");
  class_<sprokit::scheduler_registry::types_t>("SchedulerTypes"
    , "A collection of scheduler types.")
    .def(vector_indexing_suite<sprokit::scheduler_registry::types_t>())
  ;
  class_<sprokit::scheduler_registry::module_t>("SchedulerModule"
    , "The type for a scheduler module name.");

  class_<sprokit::scheduler, sprokit::scheduler_t, boost::noncopyable>("Scheduler"
    , "An abstract class which offers an interface for pipeline execution strategies."
    , no_init)
    .def("start", &sprokit::scheduler::start
      , "Start the execution of the pipeline.")
    .def("wait", &sprokit::scheduler::wait
      , "Wait until the pipeline execution is complete.")
    .def("stop", &sprokit::scheduler::stop
      , "Stop the execution of the pipeline.")
  ;

  class_<sprokit::scheduler_registry, sprokit::scheduler_registry_t, boost::noncopyable>("SchedulerRegistry"
    , "A registry of all known scheduler types."
    , no_init)
    .def("self", &sprokit::scheduler_registry::self
      , "Returns an instance of the scheduler registry.")
    .staticmethod("self")
    .def("register_scheduler", &register_scheduler
      , (arg("type"), arg("description"), arg("ctor"))
      , "Registers a function which creates a scheduler of the given type.")
    .def("create_scheduler", &sprokit::scheduler_registry::create_scheduler
      , (arg("type"), arg("pipeline"), arg("config") = sprokit::config::empty_config())
      , "Creates a new scheduler of the given type.")
    .def("types", &sprokit::scheduler_registry::types
      , "A list of known scheduler types.")
    .def("description", &sprokit::scheduler_registry::description
      , (arg("type"))
      , "The description for the given scheduler type.")
    .def("is_module_loaded", &sprokit::scheduler_registry::is_module_loaded
      , (arg("module"))
      , "Returns True if the module has already been loaded, False otherwise.")
    .def("mark_module_as_loaded", &sprokit::scheduler_registry::mark_module_as_loaded
      , (arg("module"))
      , "Marks a module as loaded.")
    .def_readonly("default_type", &sprokit::scheduler_registry::default_type
      , "The default scheduler type.")
  ;
}

class python_scheduler_wrapper
  : sprokit::python::python_threading
{
  public:
    python_scheduler_wrapper(object obj);
    ~python_scheduler_wrapper();

    sprokit::scheduler_t operator () (sprokit::pipeline_t const& pipeline, sprokit::config_t const& config);
  private:
    object const m_obj;
};

void
register_scheduler(sprokit::scheduler_registry_t reg,
                  sprokit::scheduler_registry::type_t const& type,
                  sprokit::scheduler_registry::description_t const& desc,
                  object obj)
{
  sprokit::python::python_gil const gil;

  (void)gil;

  python_scheduler_wrapper const wrap(obj);

  reg->register_scheduler(type, desc, wrap);
}

python_scheduler_wrapper
::python_scheduler_wrapper(object obj)
  : m_obj(obj)
{
}

python_scheduler_wrapper
::~python_scheduler_wrapper()
{
}

sprokit::scheduler_t
python_scheduler_wrapper
::operator () (sprokit::pipeline_t const& pipeline, sprokit::config_t const& config)
{
  sprokit::python::python_gil const gil;

  (void)gil;

  return extract<sprokit::scheduler_t>(m_obj(pipeline, config));
}
