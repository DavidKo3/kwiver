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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
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

#ifndef PLUGIN_LOADER_PLUGIN_REGISTRAR_H
#define PLUGIN_LOADER_PLUGIN_REGISTRAR_H

#include <vital/plugin_loader/plugin_factory.h>

// ==================================================================
// Support for adding factories for the plugin loader

namespace kwiver {
namespace vital {
  class plugin_loader;
} // end namespace vitak


/// Class to assist in registering tools.
class plugin_registrar
{
public:
  /**
   * @brief Create registrar
   *
   * This class contains the common data used for registering tools.
   *
   * @param vpl Reference to the plugin loader
   * @param name Name of this loadable module.
   */
  plugin_registrar( vital::plugin_loader& vpl,
                    const std::string& name )
    : mod_name( name )

#if defined PLUGIN_ORG
    , mod_organization( PLUGIN_ORG )
#else
    , mod_organization( "Unspecified" )
#endif
    , m_plugin_loader( vpl )
  {
  }

  bool is_module_loaded() { return m_plugin_loader.is_module_loaded( mod_name ); }
  void mark_module_as_loaded() { m_plugin_loader.mark_module_as_loaded( mod_name ); }

  const std::string& module_name() const { return this->mod_name; }
  const std::string& organization() const { return this->mod_organization; }
  kwiver::vital::plugin_loader& plugin_loader() { return this->m_plugin_loader; }

private:
  const std::string mod_name;
  const std::string mod_organization;

  kwiver::vital::plugin_loader& m_plugin_loader;
};

} // end namespace

#endif // PLUGIN_LOADER_PLUGIN_REGISTRAR_H
