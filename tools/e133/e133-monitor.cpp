/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * e133-monitor.cpp
 * Copyright (C) 2011 Simon Newton
 *
 * This locates all E1.33 devices using SLP and then opens a TCP connection to
 * each.  If --targets is used it skips the SLP step.
 *
 * It then waits to receive E1.33 messages on the TCP connections.
 */

#include "plugins/e131/e131/E131Includes.h"  //  NOLINT, this has to be first
#include <errno.h>
#include <getopt.h>
#include <sysexits.h>

#include <ola/BaseTypes.h>
#include <ola/Callback.h>
#include <ola/Logging.h>
#include <ola/StringUtils.h>
#include <ola/base/Flags.h>
#include <ola/e133/E133URLParser.h>
#include <ola/e133/OLASLPThread.h>
#ifdef HAVE_LIBSLP
#include <ola/e133/OpenSLPThread.h>
#endif
#include <ola/io/SelectServer.h>
#include <ola/io/StdinHandler.h>
#include <ola/network/IPV4Address.h>
#include <ola/rdm/CommandPrinter.h>
#include <ola/rdm/PidStoreHelper.h>
#include <ola/rdm/RDMCommand.h>
#include <ola/rdm/RDMHelper.h>
#include <ola/rdm/UID.h>
#include <ola/slp/URLEntry.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "plugins/e131/e131/CID.h"

#include "tools/e133/DeviceManager.h"
#include "tools/e133/MessageBuilder.h"

using ola::NewCallback;
using ola::network::IPV4Address;
using ola::rdm::PidStoreHelper;
using ola::rdm::RDMCommand;
using ola::rdm::UID;
using ola::slp::URLEntries;
using std::auto_ptr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

#ifdef HAVE_LIBSLP
DEFINE_bool(openslp, false, "Use openslp rather than the OLA SLP server");
#endif
DEFINE_s_int8(log_level, l, ola::OLA_LOG_WARN, "Set the logging level 0 .. 4.");
DEFINE_s_string(pid_location, p, PID_DATA_DIR,
                "The directory to read PID definitiions from");
DEFINE_s_string(target_addresses, t, "",
                "List of IPs to connect to, overrides SLP");


/**
 * A very simple E1.33 Controller that acts as a passive monitor.
 */
class SimpleE133Monitor {
  public:
    enum SLPOption {
      OPEN_SLP,
      OLA_SLP,
      NO_SLP,
    };
    explicit SimpleE133Monitor(PidStoreHelper *pid_helper,
                               SLPOption slp_option);
    ~SimpleE133Monitor();

    bool Init();
    void AddIP(const IPV4Address &ip_address);

    void Run() { m_ss.Run(); }

  private:
    ola::rdm::CommandPrinter m_command_printer;
    ola::io::SelectServer m_ss;
    ola::io::StdinHandler m_stdin_handler;
    auto_ptr<ola::e133::BaseSLPThread> m_slp_thread;

    MessageBuilder m_message_builder;
    DeviceManager m_device_manager;

    void Input(char c);
    void DiscoveryCallback(bool status, const URLEntries &urls);

    bool EndpointRequest(
        const ola::plugin::e131::TransportHeader &transport_header,
        const ola::plugin::e131::E133Header &e133_header,
        const string &raw_request);
};


/**
 * Setup a new Monitor
 */
SimpleE133Monitor::SimpleE133Monitor(
    PidStoreHelper *pid_helper,
    SLPOption slp_option)
    : m_command_printer(&cout, pid_helper),
      m_stdin_handler(&m_ss,
                      ola::NewCallback(this, &SimpleE133Monitor::Input)),
      m_message_builder(ola::plugin::e131::CID::Generate(), "OLA Monitor"),
      m_device_manager(&m_ss, &m_message_builder) {
  if (slp_option == OLA_SLP) {
    m_slp_thread.reset(new ola::e133::OLASLPThread(&m_ss));
  } else if (slp_option == OPEN_SLP) {
#ifdef HAVE_LIBSLP
    m_slp_thread.reset(new ola::e133::OpenSLPThread(&m_ss));
#else
    OLA_WARN << "openslp not installed";
#endif
  }
  if (m_slp_thread.get()) {
    m_slp_thread->SetNewDeviceCallback(
      NewCallback(this, &SimpleE133Monitor::DiscoveryCallback));
  }

  m_device_manager.SetRDMMessageCallback(
      NewCallback(this, &SimpleE133Monitor::EndpointRequest));

  // TODO(simon): add a controller discovery callback here as well.
}


SimpleE133Monitor::~SimpleE133Monitor() {
  if (m_slp_thread.get()) {
    m_slp_thread->Join(NULL);
    m_slp_thread->Cleanup();
  }
}


bool SimpleE133Monitor::Init() {
  if (!m_slp_thread.get())
    return true;

  if (!m_slp_thread->Init()) {
    OLA_WARN << "SLPThread Init() failed";
    return false;
  }

  m_slp_thread->Start();
  return true;
}


void SimpleE133Monitor::AddIP(const IPV4Address &ip_address) {
  m_device_manager.AddDevice(ip_address);
}


void SimpleE133Monitor::Input(char c) {
  switch (c) {
    case 'q':
      m_ss.Terminate();
      break;
    default:
      break;
  }
}


/**
 * Called when SLP completes discovery.
 */
void SimpleE133Monitor::DiscoveryCallback(bool ok, const URLEntries &urls) {
  if (ok) {
    URLEntries::const_iterator iter;
    UID uid(0, 0);
    IPV4Address ip;
    for (iter = urls.begin(); iter != urls.end(); ++iter) {
      OLA_INFO << "Located " << *iter;
      if (!ola::e133::ParseE133URL(iter->url(), &uid, &ip))
        continue;

      if (uid.IsBroadcast()) {
        OLA_WARN << "UID " << uid << "@" << ip << " is broadcast";
        continue;
      }
      AddIP(ip);
    }
  } else {
    OLA_INFO << "SLP discovery failed";
  }
}


/**
 * We received data to endpoint 0
 */
bool SimpleE133Monitor::EndpointRequest(
    const ola::plugin::e131::TransportHeader &transport_header,
    const ola::plugin::e131::E133Header&,
    const string &raw_request) {
  unsigned int slot_count = raw_request.size();
  const uint8_t *rdm_data = reinterpret_cast<const uint8_t*>(
    raw_request.data());

  cout << "From " << transport_header.Source() << ":" << endl;
  auto_ptr<RDMCommand> command(
      RDMCommand::Inflate(reinterpret_cast<const uint8_t*>(raw_request.data()),
                          raw_request.size()));
  if (command.get()) {
    command->Print(&m_command_printer, false, true);
  } else {
    ola::FormatData(&cout, rdm_data, slot_count, 2);
  }
  return true;
}


/*
 * Startup a node
 */
int main(int argc, char *argv[]) {
  ola::SetHelpString("[options]", "Monitor E1.33 Devices.");
  ola::ParseFlags(&argc, argv);

  PidStoreHelper pid_helper(string(FLAGS_pid_location), 4);

  ola::log_level log_level = ola::OLA_LOG_WARN;
  switch (FLAGS_log_level) {
    case 0:
      // nothing is written at this level
      // so this turns logging off
      log_level = ola::OLA_LOG_NONE;
      break;
    case 1:
      log_level = ola::OLA_LOG_FATAL;
      break;
    case 2:
      log_level = ola::OLA_LOG_WARN;
      break;
    case 3:
      log_level = ola::OLA_LOG_INFO;
      break;
    case 4:
      log_level = ola::OLA_LOG_DEBUG;
      break;
    default :
      break;
  }

  ola::InitLogging(log_level, ola::OLA_LOG_STDERR);

  vector<IPV4Address> targets;
  if (!string(FLAGS_target_addresses).empty()) {
    vector<string> tokens;
    ola::StringSplit(string(FLAGS_target_addresses), tokens, ",");

    vector<string>::const_iterator iter = tokens.begin();
    for (; iter != tokens.end(); ++iter) {
      IPV4Address ip_address;
      if (!IPV4Address::FromString(*iter, &ip_address)) {
        OLA_WARN << "Invalid address " << *iter;
        ola::DisplayUsage();
      }
      targets.push_back(ip_address);
    }
  }

  if (!pid_helper.Init())
    exit(EX_OSFILE);

  SimpleE133Monitor::SLPOption slp_option = SimpleE133Monitor::NO_SLP;
  if (targets.empty()) {
    slp_option = FLAGS_openslp ? SimpleE133Monitor::OPEN_SLP :
        SimpleE133Monitor::OLA_SLP;
  }
  SimpleE133Monitor monitor(&pid_helper, slp_option);
  if (!monitor.Init())
    exit(EX_UNAVAILABLE);

  if (!targets.empty()) {
    // manually add the responder IPs
    vector<IPV4Address>::const_iterator iter = targets.begin();
    for (; iter != targets.end(); ++iter)
      monitor.AddIP(*iter);
  }
  monitor.Run();
}
