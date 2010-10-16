/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * RDMHttpModule.cpp
 * This module acts as the http -> olad gateway for RDM commands.
 * Copyright (C) 2010 Simon Newton
 */

#include <algorithm>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "ola/BaseTypes.h"
#include "ola/Callback.h"
#include "ola/Logging.h"
#include "ola/OlaCallbackClient.h"
#include "ola/StringUtils.h"
#include "ola/rdm/RDMHelper.h"
#include "ola/rdm/UID.h"
#include "ola/rdm/UIDSet.h"
#include "ola/web/JsonSections.h"
#include "olad/OlaServer.h"
#include "olad/RDMHttpModule.h"


namespace ola {

using ola::rdm::UID;
using ola::web::BoolItem;
using ola::web::HiddenItem;
using ola::web::JsonSection;
using ola::web::SelectItem;
using ola::web::StringItem;
using ola::web::UIntItem;
using std::endl;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;


const char RDMHttpModule::BACKEND_DISCONNECTED_ERROR[] =
    "Failed to send request, client isn't connected";

// global url params
const char RDMHttpModule::HINT_KEY[] = "hint";
const char RDMHttpModule::ID_KEY[] = "id";
const char RDMHttpModule::SECTION_KEY[] = "section";
const char RDMHttpModule::UID_KEY[] = "uid";

// url params for particular sections
const char RDMHttpModule::ADDRESS_FIELD[] = "address";
const char RDMHttpModule::HOURS_FIELD[] = "hours";
const char RDMHttpModule::IDENTIFY_FIELD[] = "identify";
const char RDMHttpModule::LABEL_FIELD[] = "label";
const char RDMHttpModule::LANGUAGE_FIELD[] = "language";
const char RDMHttpModule::RECORD_SENSOR_FIELD[] = "record";

// section identifiers
const char RDMHttpModule::BOOT_SOFTWARE_SECTION[] = "boot_software";
const char RDMHttpModule::DEVICE_HOURS_SECTION[] = "device_hours";
const char RDMHttpModule::DEVICE_INFO_SECTION[] = "device_info";
const char RDMHttpModule::DEVICE_LABEL_SECTION[] = "device_label";
const char RDMHttpModule::DMX_ADDRESS_SECTION[] = "dmx_address";
const char RDMHttpModule::IDENTIFY_SECTION[] = "identify";
const char RDMHttpModule::LAMP_HOURS_SECTION[] = "lamp_hours";
const char RDMHttpModule::LANGUAGE_SECTION[] = "language";
const char RDMHttpModule::MANUFACTURER_LABEL_SECTION[] = "manufacturer_label";
const char RDMHttpModule::PRODUCT_DETAIL_SECTION[] = "product_detail";
const char RDMHttpModule::SENSOR_SECTION[] = "sensor";

/**
 * Create a new OLA HTTP server
 * @param export_map the ExportMap to display when /debug is called
 * @param client_socket A ConnectedSocket which is used to communicate with the
 *   server.
 * @param
 */
RDMHttpModule::RDMHttpModule(HttpServer *http_server,
                             OlaCallbackClient *client)
    : HttpModule(http_server, client),
      m_server(http_server),
      m_client(client),
      m_rdm_api(m_client) {

  m_server->RegisterHandler(
      "/rdm/run_discovery",
      NewCallback(this, &RDMHttpModule::RunRDMDiscovery));
  m_server->RegisterHandler(
      "/json/rdm/uids",
      NewCallback(this, &RDMHttpModule::JsonUIDs));
  m_server->RegisterHandler(
      "/json/rdm/supported_pids",
      NewCallback(this, &RDMHttpModule::JsonSupportedPIDs));
  m_server->RegisterHandler(
      "/json/rdm/supported_sections",
      NewCallback(this, &RDMHttpModule::JsonSupportedSections));
  m_server->RegisterHandler(
      "/json/rdm/section_info",
      NewCallback(this, &RDMHttpModule::JsonSectionInfo));
  m_server->RegisterHandler(
      "/json/rdm/set_section_info",
      NewCallback(this, &RDMHttpModule::JsonSaveSectionInfo));
}


/*
 * Teardown
 */
RDMHttpModule::~RDMHttpModule() {
  map<unsigned int, uid_resolution_state*>::iterator uid_iter;
  for (uid_iter = m_universe_uids.begin(); uid_iter != m_universe_uids.end();
       uid_iter++) {
    delete uid_iter->second;
  }
  m_universe_uids.clear();
}


/**
 * Run RDM discovery for a universe
 * @param request the HttpRequest
 * @param response the HttpResponse
 * @returns MHD_NO or MHD_YES
 */
int RDMHttpModule::RunRDMDiscovery(const HttpRequest *request,
                                   HttpResponse *response) {
  unsigned int universe_id;
  if (!CheckForInvalidId(request, &universe_id))
    return m_server->ServeNotFound(response);

  bool ok = m_client->ForceDiscovery(
      universe_id,
      NewSingleCallback(this,
                        &RDMHttpModule::HandleBoolResponse,
                        response));

  if (!ok)
    return m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR);
  return MHD_YES;
}


/**
 * Return the list of uids for this universe as json
 * @param request the HttpRequest
 * @param response the HttpResponse
 * @returns MHD_NO or MHD_YES
 */
int RDMHttpModule::JsonUIDs(const HttpRequest *request,
                            HttpResponse *response) {
  unsigned int universe_id;
  if (!CheckForInvalidId(request, &universe_id))
    return m_server->ServeNotFound(response);

  bool ok = m_client->FetchUIDList(
      universe_id,
      NewSingleCallback(this,
                        &RDMHttpModule::HandleUIDList,
                        response,
                        universe_id));

  if (!ok)
    return m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR);
  return MHD_YES;
}


/**
 * Return a list of pids supported by this device. This isn't used by the UI
 * but it's useful for debugging.
 * @param request the HttpRequest
 * @param response the HttpResponse
 * @returns MHD_NO or MHD_YES
 */
int RDMHttpModule::JsonSupportedPIDs(const HttpRequest *request,
                                     HttpResponse *response) {
  unsigned int universe_id;
  if (!CheckForInvalidId(request, &universe_id))
    return m_server->ServeNotFound(response);

  UID *uid = NULL;
  if (!CheckForInvalidUid(request, &uid))
    return m_server->ServeNotFound(response);

  string error;
  bool ok = m_rdm_api.GetSupportedParameters(
      universe_id,
      *uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::SupportedParamsHandler,
                        response),
      &error);
  delete uid;

  if (!ok)
    return m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR);
  return MHD_YES;
}


/**
 * Return a list of sections to display in the RDM control panel.
 * We use the response from SUPPORTED_PARAMS and DEVICE_INFO to decide which
 * pids exist.
 * @param request the HttpRequest
 * @param response the HttpResponse
 * @returns MHD_NO or MHD_YES
 */
int RDMHttpModule::JsonSupportedSections(const HttpRequest *request,
                                         HttpResponse *response) {
  unsigned int universe_id;
  if (!CheckForInvalidId(request, &universe_id))
    return m_server->ServeNotFound(response);

  UID *uid = NULL;
  if (!CheckForInvalidUid(request, &uid))
    return m_server->ServeNotFound(response);

  string error;
  bool ok = m_rdm_api.GetSupportedParameters(
      universe_id,
      *uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::SupportedSectionsHandler,
                        response,
                        universe_id,
                        *uid),
      &error);
  delete uid;

  if (!ok)
    return m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR);
  return MHD_YES;
}


/**
 * Get the information required to render a section in the RDM controller panel
 */
int RDMHttpModule::JsonSectionInfo(const HttpRequest *request,
                                   HttpResponse *response) {
  unsigned int universe_id;
  if (!CheckForInvalidId(request, &universe_id))
    return m_server->ServeNotFound(response);

  UID *uid = NULL;
  if (!CheckForInvalidUid(request, &uid))
    return m_server->ServeNotFound(response);

  string section_id = request->GetParameter(SECTION_KEY);
  string error;
  if (section_id == DEVICE_INFO_SECTION) {
    error = GetDeviceInfo(request, response, universe_id, *uid);
  } else if (section_id == PRODUCT_DETAIL_SECTION) {
    error = GetProductIds(request, response, universe_id, *uid);
  } else if (section_id == MANUFACTURER_LABEL_SECTION) {
    error = GetManufacturerLabel(request, response, universe_id, *uid);
  } else if (section_id == DEVICE_LABEL_SECTION) {
    error = GetDeviceLabel(request, response, universe_id, *uid);
  } else if (section_id == LANGUAGE_SECTION) {
    error = GetLanguage(response, universe_id, *uid);
  } else if (section_id == BOOT_SOFTWARE_SECTION) {
    error = GetBootSoftware(response, universe_id, *uid);
  } else if (section_id == DMX_ADDRESS_SECTION) {
    error = GetStartAddress(request, response, universe_id, *uid);
  } else if (section_id == SENSOR_SECTION) {
    error = GetSensor(request, response, universe_id, *uid);
  } else if (section_id == DEVICE_HOURS_SECTION) {
    error = GetDeviceHours(request, response, universe_id, *uid);
  } else if (section_id == LAMP_HOURS_SECTION) {
    error = GetLampHours(request, response, universe_id, *uid);
  } else if (section_id == IDENTIFY_SECTION) {
    error = GetIdentifyMode(response, universe_id, *uid);
  } else {
    OLA_INFO << "Missing or unknown section id: " << section_id;
    return m_server->ServeNotFound(response);
  }

  if (!error.empty())
    return m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
  return MHD_YES;
}


/**
 * Save the information for a section or item.
 */
int RDMHttpModule::JsonSaveSectionInfo(const HttpRequest *request,
                                       HttpResponse *response) {
  unsigned int universe_id;
  if (!CheckForInvalidId(request, &universe_id))
    return m_server->ServeNotFound(response);

  UID *uid = NULL;
  if (!CheckForInvalidUid(request, &uid))
    return m_server->ServeNotFound(response);

  string section_id = request->GetParameter(SECTION_KEY);
  string error;
  if (section_id == DEVICE_LABEL_SECTION) {
    error = SetDeviceLabel(request, response, universe_id, *uid);
  } else if (section_id == LANGUAGE_SECTION) {
    error = SetLanguage(request, response, universe_id, *uid);
  } else if (section_id == DMX_ADDRESS_SECTION) {
    error = SetStartAddress(request, response, universe_id, *uid);
  } else if (section_id == SENSOR_SECTION) {
    error = RecordSensor(request, response, universe_id, *uid);
  } else if (section_id == DEVICE_HOURS_SECTION) {
    error = SetDeviceHours(request, response, universe_id, *uid);
  } else if (section_id == LAMP_HOURS_SECTION) {
    error = SetLampHours(request, response, universe_id, *uid);
  } else if (section_id == IDENTIFY_SECTION) {
    error = SetIdentifyMode(request, response, universe_id, *uid);
  } else {
    OLA_INFO << "Missing or unknown section id: " << section_id;
    return m_server->ServeNotFound(response);
  }

  if (!error.empty())
    return RespondWithError(response, error);
  return MHD_YES;
}


/**
 * This is called from the main http server whenever a new list of active
 * universes is received. It's used to prune the uid map so we don't bother
 * trying to resolve uids for universes that no longer exist.
 */
void RDMHttpModule::PruneUniverseList(const vector<OlaUniverse> &universes) {
  map<unsigned int, uid_resolution_state*>::iterator uid_iter;
  for (uid_iter = m_universe_uids.begin(); uid_iter != m_universe_uids.end();
       uid_iter++) {
    uid_iter->second->active = false;
  }

  vector<OlaUniverse>::const_iterator iter;
  for (iter = universes.begin(); iter != universes.end(); ++iter) {
    uid_iter = m_universe_uids.find(iter->Id());
    if (uid_iter != m_universe_uids.end())
      uid_iter->second->active = true;
  }

  // clean up the uid map for those universes that no longer exist
  for (uid_iter = m_universe_uids.begin(); uid_iter != m_universe_uids.end();) {
    if (!uid_iter->second->active) {
      OLA_DEBUG << "removing " << uid_iter->first << " from the uid map";
      delete uid_iter->second;
      m_universe_uids.erase(uid_iter++);
    } else {
      uid_iter++;
    }
  }
}


/*
 * Handle the UID list response.
 * @param response the HttpResponse that is associated with the request.
 * @param uids the UIDs for this response.
 * @param error an error string.
 */
void RDMHttpModule::HandleUIDList(HttpResponse *response,
                                  unsigned int universe_id,
                                  const ola::rdm::UIDSet &uids,
                                  const string &error) {
  if (!error.empty()) {
    m_server->ServeError(response, error);
    return;
  }
  ola::rdm::UIDSet::Iterator iter = uids.Begin();
  uid_resolution_state *uid_state = GetUniverseUidsOrCreate(universe_id);

  // mark all uids as inactive so we can remove the unused ones at the end
  map<UID, resolved_uid>::iterator uid_iter;
  for (uid_iter = uid_state->resolved_uids.begin();
       uid_iter != uid_state->resolved_uids.end(); ++uid_iter)
    uid_iter->second.active = false;

  stringstream str;
  str << "{" << endl;
  str << "  \"universe\": " << universe_id << "," << endl;
  str << "  \"uids\": [" << endl;

  for (; iter != uids.End(); ++iter) {
    uid_iter = uid_state->resolved_uids.find(*iter);

    string manufacturer = "";
    string device = "";

    if (uid_iter == uid_state->resolved_uids.end()) {
      // schedule resolution
      uid_state->pending_uids.push(
          std::pair<UID, uid_resolve_action>(*iter, RESOLVE_MANUFACTURER));
      uid_state->pending_uids.push(
          std::pair<UID, uid_resolve_action>(*iter, RESOLVE_DEVICE));
      resolved_uid uid_descriptor = {"", "", true};
      uid_state->resolved_uids[*iter] = uid_descriptor;
      OLA_DEBUG << "Adding UID " << uid_iter->first << " to resolution queue";
    } else {
      manufacturer = uid_iter->second.manufacturer;
      device = uid_iter->second.device;
      uid_iter->second.active = true;
    }
    str << "    {" << endl;
    str << "       \"manufacturer_id\": " << iter->ManufacturerId() << ","
      << endl;
    str << "       \"device_id\": " << iter->DeviceId() << "," << endl;
    str << "       \"device\": \"" << EscapeString(device) << "\"," << endl;
    str << "       \"manufacturer\": \"" << EscapeString(manufacturer) <<
      "\"," << endl;
    str << "    }," << endl;
  }

  str << "  ]" << endl;
  str << "}";

  response->SetContentType(HttpServer::CONTENT_TYPE_PLAIN);
  response->Append(str.str());
  response->Send();
  delete response;

  // remove any old uids
  for (uid_iter = uid_state->resolved_uids.begin();
       uid_iter != uid_state->resolved_uids.end();) {
    if (!uid_iter->second.active) {
      OLA_DEBUG << "Removed UID " << uid_iter->first;
      uid_state->resolved_uids.erase(uid_iter++);
    } else {
      ++uid_iter;
    }
  }

  if (!uid_state->uid_resolution_running)
    ResolveNextUID(universe_id);
}


/*
 * Send the RDM command needed to resolve the next uid in the queue
 * @param universe_id the universe id to resolve the next UID for.
 */
void RDMHttpModule::ResolveNextUID(unsigned int universe_id) {
  bool sent_request = false;
  string error;
  uid_resolution_state *uid_state = GetUniverseUids(universe_id);

  if (!uid_state)
    return;

  while (!sent_request) {
    if (!uid_state->pending_uids.size()) {
      uid_state->uid_resolution_running = false;
      return;
    }
    uid_state->uid_resolution_running = true;

    pair<UID, uid_resolve_action> &uid_action_pair =
      uid_state->pending_uids.front();
    if (uid_action_pair.second == RESOLVE_MANUFACTURER) {
      OLA_DEBUG << "sending manufacturer request for " << uid_action_pair.first;
      sent_request = m_rdm_api.GetManufacturerLabel(
          universe_id,
          uid_action_pair.first,
          ola::rdm::ROOT_RDM_DEVICE,
          NewSingleCallback(this,
                            &RDMHttpModule::UpdateUIDManufacturerLabel,
                            universe_id,
                            uid_action_pair.first),
          &error);
      OLA_DEBUG << "return code was " << sent_request;
      uid_state->pending_uids.pop();
    } else if (uid_action_pair.second == RESOLVE_DEVICE) {
      OLA_INFO << "sending device request for " << uid_action_pair.first;
      sent_request = m_rdm_api.GetDeviceLabel(
          universe_id,
          uid_action_pair.first,
          ola::rdm::ROOT_RDM_DEVICE,
          NewSingleCallback(this,
                            &RDMHttpModule::UpdateUIDDeviceLabel,
                            universe_id,
                            uid_action_pair.first),
          &error);
      uid_state->pending_uids.pop();
      OLA_DEBUG << "return code was " << sent_request;
    } else {
      OLA_WARN << "Unknown UID resolve action " <<
        static_cast<int>(uid_action_pair.second);
    }
  }
}

/*
 * Handle the manufacturer label response.
 */
void RDMHttpModule::UpdateUIDManufacturerLabel(
    unsigned int universe,
    UID uid,
    const ola::rdm::ResponseStatus &status,
    const string &manufacturer_label) {
  uid_resolution_state *uid_state = GetUniverseUids(universe);

  if (!uid_state)
    return;

  if (CheckForRDMSuccess(status)) {
    map<UID, resolved_uid>::iterator uid_iter;
    uid_iter = uid_state->resolved_uids.find(uid);
    if (uid_iter != uid_state->resolved_uids.end())
      uid_iter->second.manufacturer = manufacturer_label;
  }
  ResolveNextUID(universe);
}


/*
 * Handle the device label response.
 */
void RDMHttpModule::UpdateUIDDeviceLabel(
    unsigned int universe,
    UID uid,
    const ola::rdm::ResponseStatus &status,
    const string &device_label) {
  uid_resolution_state *uid_state = GetUniverseUids(universe);

  if (!uid_state)
    return;

  if (CheckForRDMSuccess(status)) {
    map<UID, resolved_uid>::iterator uid_iter;
    uid_iter = uid_state->resolved_uids.find(uid);
    if (uid_iter != uid_state->resolved_uids.end())
      uid_iter->second.device = device_label;
  }
  ResolveNextUID(universe);
}


/*
 * Get the UID resolution state for a particular universe
 * @param universe the id of the universe to get the state for
 */
RDMHttpModule::uid_resolution_state *RDMHttpModule::GetUniverseUids(
    unsigned int universe) {
  map<unsigned int, uid_resolution_state*>::iterator iter =
    m_universe_uids.find(universe);
  return iter == m_universe_uids.end() ? NULL : iter->second;
}


/*
 * Get the UID resolution state for a particular universe or create one if it
 * doesn't exist.
 * @param universe the id of the universe to get the state for
 */
RDMHttpModule::uid_resolution_state *RDMHttpModule::GetUniverseUidsOrCreate(
    unsigned int universe) {
  map<unsigned int, uid_resolution_state*>::iterator iter =
    m_universe_uids.find(universe);

  if (iter == m_universe_uids.end()) {
    OLA_DEBUG << "Adding a new state entry for " << universe;
    uid_resolution_state *state  = new uid_resolution_state();
    state->uid_resolution_running = false;
    state->active = true;
    pair<unsigned int, uid_resolution_state*> p(universe, state);
    iter = m_universe_uids.insert(p).first;
  }
  return iter->second;
}


/*
 * Handle the response from a supported params request
 */
void RDMHttpModule::SupportedParamsHandler(
    HttpResponse *response,
    const ola::rdm::ResponseStatus &status,
    const vector<uint16_t> &pids) {
  stringstream str;
  if (CheckForRDMSuccess(status)) {
    vector<uint16_t>::const_iterator iter = pids.begin();

    str << "{" << endl;
    str << "  \"pids\": [" << endl;

    for (; iter != pids.end(); ++iter) {
      str << "    0x" << std::hex << *iter << ",\n";
    }

    str << "  ]" << endl;
    str << "}";
  }

  response->SetContentType(HttpServer::CONTENT_TYPE_PLAIN);
  response->Append(str.str());
  response->Send();
  delete response;
}


/**
 * Takes the supported pids for a device and come up with the list of sections
 * to display in the RDM panel
 */
void RDMHttpModule::SupportedSectionsHandler(
    HttpResponse *response,
    unsigned int universe_id,
    UID uid,
    const ola::rdm::ResponseStatus &status,
    const vector<uint16_t> &pid_list) {
  string error;

  // nacks here are ok if the device doesn't support SUPPORTED_PARAMS
  if (!CheckForRDMSuccess(status) &&
      status.ResponseType() != ola::rdm::ResponseStatus::REQUEST_NACKED) {
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
    return;
  }

  m_rdm_api.GetDeviceInfo(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::SupportedSectionsDeviceInfoHandler,
                        response,
                        pid_list),
      &error);
  if (!error.empty())
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
}


/**
 * Handle the second part of the supported sections request.
 */
void RDMHttpModule::SupportedSectionsDeviceInfoHandler(
    HttpResponse *response,
    const vector<uint16_t> pid_list,
    const ola::rdm::ResponseStatus &status,
    const ola::rdm::DeviceDescriptor &device) {
  vector<section_info> sections;
  std::set<uint16_t> pids;
  copy(pid_list.begin(), pid_list.end(), inserter(pids, pids.end()));

  // PID_DEVICE_INFO is required so we always add it
  string hint;
  if (pids.find(ola::rdm::PID_DEVICE_MODEL_DESCRIPTION) != pids.end())
      hint.push_back('m');  // m is for device model
  AddSection(&sections, DEVICE_INFO_SECTION, "Device Info", hint);

  AddSection(&sections, IDENTIFY_SECTION, "Identify Mode", hint);

  bool dmx_address_added = false;
  bool include_software_version = false;
  vector<uint16_t>::const_iterator iter = pid_list.begin();
  for (; iter != pid_list.end(); ++iter) {
    switch (*iter) {
      case ola::rdm::PID_MANUFACTURER_LABEL:
        AddSection(&sections, MANUFACTURER_LABEL_SECTION,
                   "Manufacturer Label");
        break;
      case ola::rdm::PID_DEVICE_LABEL:
        AddSection(&sections, DEVICE_LABEL_SECTION, "Device Label");
        break;
      case ola::rdm::PID_LANGUAGE:
        AddSection(&sections, LANGUAGE_SECTION, "Language");
        break;
      case ola::rdm::PID_BOOT_SOFTWARE_VERSION_ID:
      case ola::rdm::PID_BOOT_SOFTWARE_VERSION_LABEL:
        include_software_version = true;
        break;
      case ola::rdm::PID_DMX_START_ADDRESS:
        AddSection(&sections, DMX_ADDRESS_SECTION, "DMX Start Address");
        dmx_address_added = true;
        break;
      case ola::rdm::PID_DEVICE_HOURS:
        AddSection(&sections, DEVICE_HOURS_SECTION, "Device Hours");
        break;
      case ola::rdm::PID_LAMP_HOURS:
        AddSection(&sections, DEVICE_HOURS_SECTION, "Lamp Hours");
        break;
      case ola::rdm::PID_PRODUCT_DETAIL_ID_LIST:
        AddSection(&sections, PRODUCT_DETAIL_SECTION, "Product Details");
        break;
    }
  }

  if (include_software_version)
    AddSection(&sections, BOOT_SOFTWARE_SECTION, "Boot Software Version");

  if (CheckForRDMSuccess(status)) {
    if (device.dmx_footprint && !dmx_address_added)
      AddSection(&sections, DMX_ADDRESS_SECTION, "DMX Start Address");
    if (device.sensor_count &&
        pids.find(ola::rdm::PID_SENSOR_DEFINITION) != pids.end() &&
        pids.find(ola::rdm::PID_SENSOR_VALUE) != pids.end()) {
      // sensors count from 1
      for (unsigned int i = 0; i < device.sensor_count; ++i) {
        stringstream heading, hint;
        hint << i;
        heading << "Sensor " << (i + 1);
        AddSection(&sections, SENSOR_SECTION, heading.str(), hint.str());
      }
    }
  }

  sort(sections.begin(), sections.end(), lt_section_info());

  vector<section_info>::const_iterator section_iter;
  stringstream str;
  str << "[" << endl;
  for (section_iter = sections.begin(); section_iter != sections.end();
       ++section_iter) {
    str << "  {" << endl;
    str << "    \"id\": \"" << section_iter->id << "\"," << endl;
    str << "    \"name\": \"" << section_iter->name << "\"," << endl;
    str << "    \"hint\": \"" << section_iter->hint << "\"," << endl;
    str << "  }," << endl;
  }
  str << "]" << endl;
  response->SetContentType(HttpServer::CONTENT_TYPE_PLAIN);
  response->Append(str.str());
  response->Send();
  delete response;
}


/*
 * Handle the request for the device info section.
 */
string RDMHttpModule::GetDeviceInfo(const HttpRequest *request,
                                    HttpResponse *response,
                                    unsigned int universe_id,
                                    const UID &uid) {
  string hint = request->GetParameter(HINT_KEY);
  string error;
  device_info dev_info = {universe_id, uid, hint, "", ""};

  m_rdm_api.GetSoftwareVersionLabel(
    universe_id,
    uid,
    ola::rdm::ROOT_RDM_DEVICE,
    NewSingleCallback(this,
                      &RDMHttpModule::GetSoftwareVersionHandler,
                      response,
                      dev_info),
    &error);
  return error;
}


/**
 * Handle the response to a software version call.
 */
void RDMHttpModule::GetSoftwareVersionHandler(
    HttpResponse *response,
    device_info dev_info,
    const ola::rdm::ResponseStatus &status,
    const string &software_version) {
  string error;

  if (CheckForRDMSuccess(status))
    dev_info.software_version = software_version;

  if (dev_info.hint.find('m') != string::npos) {
    m_rdm_api.GetDeviceModelDescription(
        dev_info.universe_id,
        dev_info.uid,
        ola::rdm::ROOT_RDM_DEVICE,
        NewSingleCallback(this,
                          &RDMHttpModule::GetDeviceModelHandler,
                          response,
                          dev_info),
        &error);
  } else {
    m_rdm_api.GetDeviceInfo(
        dev_info.universe_id,
        dev_info.uid,
        ola::rdm::ROOT_RDM_DEVICE,
        NewSingleCallback(this,
                          &RDMHttpModule::GetDeviceInfoHandler,
                          response,
                          dev_info),
        &error);
  }

  if (!error.empty())
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
}


/**
 * Handle the response to a device model call.
 */
void RDMHttpModule::GetDeviceModelHandler(
    HttpResponse *response,
    device_info dev_info,
    const ola::rdm::ResponseStatus &status,
    const string &device_model) {
  string error;

  if (CheckForRDMSuccess(status))
    dev_info.device_model = device_model;

  m_rdm_api.GetDeviceInfo(
      dev_info.universe_id,
      dev_info.uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetDeviceInfoHandler,
                        response,
                        dev_info),
      &error);

  if (!error.empty())
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
}


/**
 * Handle the response to a device info call and build the response
 */
void RDMHttpModule::GetDeviceInfoHandler(
    HttpResponse *response,
    device_info dev_info,
    const ola::rdm::ResponseStatus &status,
    const ola::rdm::DeviceDescriptor &device) {
  JsonSection section;

  if (CheckForRDMError(response, status))
    return;

  stringstream stream;
  stream << static_cast<int>(device.protocol_version_high) << "."
    << static_cast<int>(device.protocol_version_low);
  section.AddItem(new StringItem("Protocol Version", stream.str()));

  stream.str("");
  if (dev_info.device_model.empty())
    stream << device.device_model;
  else
    stream << dev_info.device_model << " (" << device.device_model << ")";
  section.AddItem(new StringItem("Device Model", stream.str()));

  section.AddItem(new StringItem(
      "Product Category",
      ola::rdm::ProductCategoryToString(device.product_category)));
  stream.str("");
  if (dev_info.software_version.empty())
    stream << device.software_version;
  else
    stream << dev_info.software_version << " (" << device.software_version
      << ")";
  section.AddItem(new StringItem("Software Version", stream.str()));
  section.AddItem(new UIntItem("DMX Footprint", device.dmx_footprint));

  stream.str("");
  stream << static_cast<int>(device.current_personality) << " of " <<
    static_cast<int>(device.personaility_count);
  section.AddItem(new StringItem("Personality", stream.str()));

  section.AddItem(new UIntItem("Sub Devices", device.sub_device_count));
  section.AddItem(new UIntItem("Sensors", device.sensor_count));
  RespondWithSection(response, section);
}


/*
 * Handle the request for the product details ids.
 */
string RDMHttpModule::GetProductIds(const HttpRequest *request,
                                    HttpResponse *response,
                                    unsigned int universe_id,
                                    const UID &uid) {
  string error;
  m_rdm_api.GetProductDetailIdList(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetProductIdsHandler,
                        response),
      &error);
  return error;
  (void) request;
}


/**
 * Handle the response to a product detail ids call and build the response.
 */
void RDMHttpModule::GetProductIdsHandler(
    HttpResponse *response,
    const ola::rdm::ResponseStatus &status,
    const vector<uint16_t> &ids) {
  if (CheckForRDMError(response, status))
    return;

  bool first = true;
  stringstream product_ids;
  JsonSection section;
  vector<uint16_t>::const_iterator iter = ids.begin();
  for (; iter != ids.end(); ++iter) {
    string product_id = ola::rdm::ProductDetailToString(*iter);
    if (product_id.empty())
      continue;

    if (first)
      first = false;
    else
      product_ids << ", ";
    product_ids << product_id;
  }
  section.AddItem(new StringItem("Product Detail IDs", product_ids.str()));
  RespondWithSection(response, section);
}


/**
 * Handle the request for the Manufacturer label.
 */
string RDMHttpModule::GetManufacturerLabel(const HttpRequest *request,
                                           HttpResponse *response,
                                           unsigned int universe_id,
                                           const UID &uid) {
  string error;
  m_rdm_api.GetManufacturerLabel(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetManufacturerLabelHandler,
                        response,
                        universe_id,
                        uid),
      &error);
  return error;
  (void) request;
}


/**
 * Handle the response to a manufacturer label call and build the response
 */
void RDMHttpModule::GetManufacturerLabelHandler(
    HttpResponse *response,
    unsigned int universe_id,
    const UID uid,
    const ola::rdm::ResponseStatus &status,
    const string &label) {
  if (CheckForRDMError(response, status))
    return;
  JsonSection section;
  section.AddItem(new StringItem("Manufacturer Label", label));
  RespondWithSection(response, section);

  // update the map as well
  uid_resolution_state *uid_state = GetUniverseUids(universe_id);
  if (uid_state) {
    map<UID, resolved_uid>::iterator uid_iter =
      uid_state->resolved_uids.find(uid);
    if (uid_iter != uid_state->resolved_uids.end())
      uid_iter->second.manufacturer = label;
  }
}


/**
 * Handle the request for the Device label.
 */
string RDMHttpModule::GetDeviceLabel(const HttpRequest *request,
                                     HttpResponse *response,
                                     unsigned int universe_id,
                                     const UID &uid) {
  string error;
  m_rdm_api.GetDeviceLabel(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetDeviceLabelHandler,
                        response,
                        universe_id,
                        uid),
      &error);
  return error;
  (void) request;
}


/**
 * Handle the response to a device label call and build the response
 */
void RDMHttpModule::GetDeviceLabelHandler(
    HttpResponse *response,
    unsigned int universe_id,
    const UID uid,
    const ola::rdm::ResponseStatus &status,
    const string &label) {
  if (CheckForRDMError(response, status))
    return;

  JsonSection section;
  section.AddItem(new StringItem("Device Label", label, LABEL_FIELD));
  RespondWithSection(response, section);

  // update the map as well
  uid_resolution_state *uid_state = GetUniverseUids(universe_id);
  if (uid_state) {
    map<UID, resolved_uid>::iterator uid_iter =
      uid_state->resolved_uids.find(uid);
    if (uid_iter != uid_state->resolved_uids.end())
      uid_iter->second.device = label;
  }
}


/*
 * Set the device label
 */
string RDMHttpModule::SetDeviceLabel(const HttpRequest *request,
                                     HttpResponse *response,
                                     unsigned int universe_id,
                                     const UID &uid) {
  string label = request->GetParameter(LABEL_FIELD);
  string error;
  m_rdm_api.SetDeviceLabel(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      label,
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the request for the language section.
 */
string RDMHttpModule::GetLanguage(HttpResponse *response,
                                  unsigned int universe_id,
                                  const UID &uid) {
  string error;
  m_rdm_api.GetLanguageCapabilities(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetSupportedLanguagesHandler,
                        response,
                        universe_id,
                        uid),
      &error);
  return error;
}


/**
 * Handle the response to language capability call.
 */
void RDMHttpModule::GetSupportedLanguagesHandler(
    HttpResponse *response,
    unsigned int universe_id,
    const UID uid,
    const ola::rdm::ResponseStatus &status,
    const vector<string> &languages) {
  string error;
  m_rdm_api.GetLanguage(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetLanguageHandler,
                        response,
                        languages),
      &error);

  if (!error.empty())
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
  (void) status;
}


/**
 * Handle the response to language call and build the response
 */
void RDMHttpModule::GetLanguageHandler(HttpResponse *response,
                                       vector<string> languages,
                                       const ola::rdm::ResponseStatus &status,
                                       const string &language) {
  JsonSection section;
  SelectItem *item = new SelectItem("Language", LANGUAGE_FIELD);
  bool ok = CheckForRDMSuccess(status);

  vector<string>::const_iterator iter = languages.begin();
  unsigned int i = 0;
  for (; iter != languages.end(); ++iter, i++) {
    item->AddItem(*iter, *iter);
    if (ok && *iter == language)
      item->SetSelectedOffset(i);
  }

  if (ok && !languages.size()) {
    item->AddItem(language, language);
    item->SetSelectedOffset(0);
  }
  section.AddItem(item);
  RespondWithSection(response, section);
}


/*
 * Set the language
 */
string RDMHttpModule::SetLanguage(const HttpRequest *request,
                                  HttpResponse *response,
                                  unsigned int universe_id,
                                  const UID &uid) {
  string label = request->GetParameter(LANGUAGE_FIELD);
  string error;
  m_rdm_api.SetLanguage(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      label,
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the request for the boot software section.
 */
string RDMHttpModule::GetBootSoftware(HttpResponse *response,
                                      unsigned int universe_id,
                                      const UID &uid) {
  string error;
  m_rdm_api.GetBootSoftwareVersionLabel(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetBootSoftwareLabelHandler,
                        response,
                        universe_id,
                        uid),
      &error);
  return error;
}


/**
 * Handle the response to a boot software label.
 */
void RDMHttpModule::GetBootSoftwareLabelHandler(
    HttpResponse *response,
    unsigned int universe_id,
    const UID uid,
    const ola::rdm::ResponseStatus &status,
    const string &label) {
  string error;
  m_rdm_api.GetBootSoftwareVersion(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetBootSoftwareVersionHandler,
                        response,
                        label),
      &error);
  if (!error.empty())
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
  (void) status;
}


/**
 * Handle the response to a boot software version.
 */
void RDMHttpModule::GetBootSoftwareVersionHandler(
    HttpResponse *response,
    string label,
    const ola::rdm::ResponseStatus &status,
    uint32_t version) {
  stringstream str;
  str << label;
  if (CheckForRDMSuccess(status)) {
    if (!label.empty())
      str << " (" << version << ")";
    else
      str << version;
  }

  JsonSection section;
  StringItem *item = new StringItem("Boot Software", str.str());
  section.AddItem(item);
  RespondWithSection(response, section);
}


/**
 * Handle the request for the start address section.
 */
string RDMHttpModule::GetStartAddress(const HttpRequest *request,
                                      HttpResponse *response,
                                      unsigned int universe_id,
                                      const UID &uid) {
  string error;
  m_rdm_api.GetDMXAddress(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetStartAddressHandler,
                        response),
      &error);
  return error;
  (void) request;
}


/**
 * Handle the response to a dmx start address call and build the response
 */
void RDMHttpModule::GetStartAddressHandler(
    HttpResponse *response,
    const ola::rdm::ResponseStatus &status,
    uint16_t address) {
  if (CheckForRDMError(response, status))
    return;

  JsonSection section;
  UIntItem *item = new UIntItem("DMX Start Address", address, ADDRESS_FIELD);
  item->SetMin(0);
  item->SetMax(DMX_UNIVERSE_SIZE - 1);
  section.AddItem(item);
  RespondWithSection(response, section);
}


/*
 * Set the DMX start address
 */
string RDMHttpModule::SetStartAddress(const HttpRequest *request,
                                      HttpResponse *response,
                                      unsigned int universe_id,
                                      const UID &uid) {
  string dmx_address = request->GetParameter(ADDRESS_FIELD);
  uint16_t address;

  if (!StringToUInt16(dmx_address, &address)) {
    return "Invalid start address";
  }

  string error;
  m_rdm_api.SetDMXAddress(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      address,
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the request for the sensor section.
 */
string RDMHttpModule::GetSensor(const HttpRequest *request,
                                HttpResponse *response,
                                unsigned int universe_id,
                                const UID &uid) {
  string hint = request->GetParameter(HINT_KEY);
  uint8_t sensor_id;
  if (!StringToUInt8(hint, &sensor_id)) {
    return "Invalid hint (sensor #)";
  }

  string error;
  m_rdm_api.GetSensorDefinition(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      sensor_id,
      NewSingleCallback(this,
                        &RDMHttpModule::SensorDefinitionHandler,
                        response,
                        universe_id,
                        uid,
                        sensor_id),
      &error);
  return error;
}


/**
 * Handle the response to a sensor definition request.
 */
void RDMHttpModule::SensorDefinitionHandler(
    HttpResponse *response,
    unsigned int universe_id,
    const UID uid,
    uint8_t sensor_id,
    const ola::rdm::ResponseStatus &status,
    const ola::rdm::SensorDescriptor &definition) {
  ola::rdm::SensorDescriptor *definition_arg = NULL;

  if (CheckForRDMSuccess(status)) {
    definition_arg = new ola::rdm::SensorDescriptor();
    *definition_arg = definition;
  }
  string error;
  m_rdm_api.GetSensorValue(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      sensor_id,
      NewSingleCallback(this,
                        &RDMHttpModule::SensorValueHandler,
                        response,
                        definition_arg),
      &error);
  if (!error.empty())
    m_server->ServeError(response, BACKEND_DISCONNECTED_ERROR + error);
}


/**
 * Handle the response to a sensor value request & build the response.
 */
void RDMHttpModule::SensorValueHandler(
    HttpResponse *response,
    ola::rdm::SensorDescriptor *definition,
    const ola::rdm::ResponseStatus &status,
    const ola::rdm::SensorValueDescriptor &value) {
  if (CheckForRDMError(response, status)) {
    if (definition)
      delete definition;
    return;
  }

  JsonSection section;
  stringstream str;
  if (definition) {
    section.AddItem(new StringItem("Description", definition->description));
    section.AddItem(new StringItem(
          "Type",
          ola::rdm::SensorTypeToString(definition->type)));
    str << definition->range_min << " - " << definition->range_max <<
      " " << ola::rdm::PrefixToString(definition->prefix) << " " <<
      ola::rdm::UnitToString(definition->unit);
    section.AddItem(new StringItem("Range", str.str()));

    str.str("");
    str << definition->normal_min << " - " << definition->normal_max <<
      " " << ola::rdm::PrefixToString(definition->prefix) << " " <<
      ola::rdm::UnitToString(definition->unit);
    section.AddItem(new StringItem("Normal Range", str.str()));

    if (definition->recorded_value_support &&
        ola::rdm::SENSOR_RECORDED_VALUE) {
      str.str("");
      str << value.recorded << " " <<
        ola::rdm::PrefixToString(definition->prefix) << " " <<
        ola::rdm::UnitToString(definition->unit);
      section.AddItem(new StringItem("Recorded Value", str.str()));
    }

    if (definition->recorded_value_support &&
        ola::rdm::SENSOR_RECORDED_RANGE_VALUES) {
      str.str("");
      str << value.lowest << " - " << value.highest <<
        " " << ola::rdm::PrefixToString(definition->prefix) << " " <<
        ola::rdm::UnitToString(definition->unit);
      section.AddItem(new StringItem("Min / Max Recorded Values", str.str()));
    }
  }
  str.str("");
  str << value.present_value << " " <<
    ola::rdm::PrefixToString(definition->prefix) << " " <<
    ola::rdm::UnitToString(definition->unit);
  section.AddItem(new StringItem("Present Value", str.str()));

  if (definition && definition->recorded_value_support) {
    section.AddItem(new HiddenItem(RECORD_SENSOR_FIELD, str.str()));
  }
  section.SetSaveButton("Record Sensor");
  RespondWithSection(response, section);
  delete definition;
}


/*
 * Record a sensor value
 */
string RDMHttpModule::RecordSensor(const HttpRequest *request,
                                   HttpResponse *response,
                                   unsigned int universe_id,
                                   const UID &uid) {
  string hint = request->GetParameter(HINT_KEY);
  uint8_t sensor_id;
  if (!StringToUInt8(hint, &sensor_id)) {
    return "Invalid hint (sensor #)";
  }

  string error;
  m_rdm_api.RecordSensors(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      sensor_id,
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the request for the device hours section.
 */
string RDMHttpModule::GetDeviceHours(const HttpRequest *request,
                                     HttpResponse *response,
                                     unsigned int universe_id,
                                     const UID &uid) {
  string error;
  m_rdm_api.GetDeviceHours(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::DeviceHoursHandler,
                        response),
      &error);
  return error;
  (void) request;
}


/**
 * Handle the response to a device hours request and build the response.
 */
void RDMHttpModule::DeviceHoursHandler(HttpResponse *response,
                                       const ola::rdm::ResponseStatus &status,
                                       uint32_t hours) {
  if (CheckForRDMError(response, status))
    return;

  JsonSection section;
  section.AddItem(new UIntItem("Device Hours", hours, HOURS_FIELD));
  RespondWithSection(response, section);
}


/**
 * Set the device hours
 */
string RDMHttpModule::SetDeviceHours(const HttpRequest *request,
                                     HttpResponse *response,
                                     unsigned int universe_id,
                                     const UID &uid) {
  string device_hours = request->GetParameter(HOURS_FIELD);
  uint32_t dev_hours;

  if (!StringToUInt(device_hours, &dev_hours)) {
    return "Invalid device hours";
  }

  string error;
  m_rdm_api.SetDeviceHours(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      dev_hours,
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the request for the device hours section.
 */
string RDMHttpModule::GetLampHours(const HttpRequest *request,
                                     HttpResponse *response,
                                     unsigned int universe_id,
                                     const UID &uid) {
  string error;
  m_rdm_api.GetLampHours(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::LampHoursHandler,
                        response),
      &error);
  return error;
  (void) request;
}


/**
 * Handle the response to a lamp hours request and build the response.
 */
void RDMHttpModule::LampHoursHandler(HttpResponse *response,
                                     const ola::rdm::ResponseStatus &status,
                                     uint32_t hours) {
  if (CheckForRDMError(response, status))
    return;

  JsonSection section;
  section.AddItem(new UIntItem("Lamp Hours", hours, HOURS_FIELD));
  RespondWithSection(response, section);
}


/**
 * Set the lamp hours
 */
string RDMHttpModule::SetLampHours(const HttpRequest *request,
                                    HttpResponse *response,
                                    unsigned int universe_id,
                                    const UID &uid) {
  string lamp_hours_str = request->GetParameter(HOURS_FIELD);
  uint32_t lamp_hours;

  if (!StringToUInt(lamp_hours_str, &lamp_hours)) {
    return "Invalid device hours";
  }

  string error;
  m_rdm_api.SetLampHours(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      lamp_hours,
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the request for the identify mode section.
 */
string RDMHttpModule::GetIdentifyMode(HttpResponse *response,
                                      unsigned int universe_id,
                                      const UID &uid) {
  string error;
  m_rdm_api.GetIdentifyMode(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      NewSingleCallback(this,
                        &RDMHttpModule::GetIdentifyModeHandler,
                        response),
      &error);
  return error;
}


/**
 * Handle the response to identify mode call and build the response
 */
void RDMHttpModule::GetIdentifyModeHandler(
    HttpResponse *response,
    const ola::rdm::ResponseStatus &status,
    bool mode) {
  if (CheckForRDMError(response, status))
    return;

  JsonSection section;
  BoolItem *item = new BoolItem("Identify Mode", mode, IDENTIFY_FIELD);
  section.AddItem(item);
  RespondWithSection(response, section);
}


/*
 * Set the idenify mode
 */
string RDMHttpModule::SetIdentifyMode(const HttpRequest *request,
                                      HttpResponse *response,
                                      unsigned int universe_id,
                                      const UID &uid) {
  string mode = request->GetParameter(IDENTIFY_FIELD);
  string error;
  m_rdm_api.IdentifyDevice(
      universe_id,
      uid,
      ola::rdm::ROOT_RDM_DEVICE,
      mode == "1",
      NewSingleCallback(this,
                        &RDMHttpModule::SetHandler,
                        response),
      &error);
  return error;
}


/**
 * Check if the id url param exists and is valid.
 */
bool RDMHttpModule::CheckForInvalidId(const HttpRequest *request,
                                      unsigned int *universe_id) {
  string uni_id = request->GetParameter(ID_KEY);
  if (!StringToUInt(uni_id, universe_id)) {
    OLA_INFO << "Invalid universe id: " << uni_id;
    return false;
  }
  return true;
}


/**
 * Check that the uid url param exists and is valid.
 */
bool RDMHttpModule::CheckForInvalidUid(const HttpRequest *request,
                                       UID **uid) {
  string uid_string = request->GetParameter(UID_KEY);
  *uid = UID::FromString(uid_string);
  if (*uid == NULL) {
    OLA_INFO << "Invalid uid: " << uid_string;
    return false;
  }
  return true;
}


/*
 * Check the response to a Set RDM call and build the response.
 */
void RDMHttpModule::SetHandler(
    HttpResponse *response,
    const ola::rdm::ResponseStatus &status) {
  string error;
  CheckForRDMSuccessWithError(status, &error);
  RespondWithError(response, error);
}


/**
 * Check for an RDM error, and if it occurs, return a json response.
 * @return true if an error occured.
 */
bool RDMHttpModule::CheckForRDMError(HttpResponse *response,
                                     const ola::rdm::ResponseStatus &status) {
  string error;
  if (!CheckForRDMSuccessWithError(status, &error)) {
    RespondWithError(response, error);
    return true;
  }
  return false;
}



int RDMHttpModule::RespondWithError(HttpResponse *response,
                                    const string &error) {
  response->SetContentType(HttpServer::CONTENT_TYPE_PLAIN);
  response->Append("{\"error\": \"" + error + "\"}");
  int r = response->Send();
  delete response;
  return r;
}


/**
 * Build & send a response from a JsonSection
 */
void RDMHttpModule::RespondWithSection(HttpResponse *response,
                                       const ola::web::JsonSection &section) {
  response->SetContentType(HttpServer::CONTENT_TYPE_PLAIN);
  response->Append(section.AsString());
  response->Send();
  delete response;
}


/*
 * Check the success of an RDM command
 * @returns true if this command was ok, false otherwise.
 */
bool RDMHttpModule::CheckForRDMSuccess(
    const ola::rdm::ResponseStatus &status) {
  string error;
  if (!CheckForRDMSuccessWithError(status, &error)) {
    OLA_INFO << error;
    return false;
  }
  return true;
}


/*
 * Check the success of an RDM command
 * @returns true if this command was ok, false otherwise.
 */
bool RDMHttpModule::CheckForRDMSuccessWithError(
    const ola::rdm::ResponseStatus &status,
    string *error) {
  stringstream str;
  switch (status.ResponseType()) {
    case ola::rdm::ResponseStatus::TRANSPORT_ERROR:
      str << "RDM command error: " << status.Error();
      if (error)
        *error = str.str();
      return false;
    case ola::rdm::ResponseStatus::BROADCAST_REQUEST:
      return false;
    case ola::rdm::ResponseStatus::REQUEST_NACKED:
      str << "Request was NACKED with code: " <<
        ola::rdm::NackReasonToString(status.NackReason());
      if (error)
        *error = str.str();
      return false;
    case ola::rdm::ResponseStatus::MALFORMED_RESPONSE:
      str << "Malformed RDM response " << status.Error();
      if (error)
        *error = str.str();
      return false;
    case ola::rdm::ResponseStatus::VALID_RESPONSE:
      return true;
    default:
      str << "Unknown response status " <<
        static_cast<int>(status.ResponseType());
      if (error)
        *error = str.str();
      return false;
  }
}


/*
 * Handle the RDM discovery response
 * @param response the HttpResponse that is associated with the request.
 * @param error an error string.
 */
void RDMHttpModule::HandleBoolResponse(HttpResponse *response,
                                       const string &error) {
  if (!error.empty()) {
    m_server->ServeError(response, error);
    return;
  }
  response->SetContentType(HttpServer::CONTENT_TYPE_PLAIN);
  response->Append("ok");
  response->Send();
  delete response;
}


/**
 * Add a section to the supported section list
 */
void RDMHttpModule::AddSection(vector<section_info> *sections,
                               const string &section_id,
                               const string &section_name,
                               const string &hint) {
  section_info info = {section_id, section_name, hint};
  sections->push_back(info);
}
}  // ola
