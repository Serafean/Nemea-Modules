/**
 * \file flow_meter.cpp
 * \brief Main file of the flow_meter module.
 * \author Vaclav Bartos <bartos@cesnet.cz>
 * \author Jiri Havranek <havraji6@fit.cvut.cz>
 * \date 2014
 * \date 2015
 * \date 2016
 */
/*
 * Copyright (C) 2014-2016 CESNET
 *
 * LICENSE TERMS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#include <config.h>
#include <getopt.h>
#include <string>
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <limits>
#include <errno.h>

#include "flow_meter.h"
#include "packet.h"
#include "flowifc.h"
#include "pcapreader.h"
#include "nhtflowcache.h"
#include "unirecexporter.h"
#include "stats.h"
#include "fields.h"

#include "httpplugin.h"
#include "dnsplugin.h"
#include "sipplugin.h"
#include "ntpplugin.h"
#include "arpplugin.h"

using namespace std;

trap_module_info_t *module_info = NULL;
static int stop = 0;

#define MODULE_BASIC_INFO(BASIC) \
  BASIC("Flow meter module", "Convert packets from PCAP file or network interface into flow records.", 0, -1)

#define MODULE_PARAMS(PARAM) \
  PARAM('p', "plugins", "Activate specified parsing plugins. Output interface for each plugin correspond the order which you specify items in -i and -p param. "\
  "For example: \'-i u:a,u:b,u:c -p http,basic,dns\' http traffic will be send to interface u:a, basic flow to u:b etc. If you don't specify -p parameter, flow meter"\
  " will require one output interface for basic flow by default. Format: plugin_name[,...] Supported plugins: http,dns,sip,ntp,basic,arp", required_argument, "string")\
  PARAM('c', "count", "Quit after number of packets are captured.", required_argument, "uint32")\
  PARAM('I', "interface", "Capture from given network interface. Parameter require interface name (eth0 for example).", required_argument, "string")\
  PARAM('r', "file", "Pcap file to read. - to read from stdin.", required_argument, "string") \
  PARAM('t', "timeout", "Active and inactive timeout in seconds. Format: DOUBLE:DOUBLE. Value default means use default value 300.0:30.0.", required_argument, "string") \
  PARAM('s', "cache_size", "Size of flow cache in number of flow records. Each flow record has 176 bytes. default means use value 65536.", required_argument, "string") \
  PARAM('S', "cache-statistics", "Print flow cache statistics. NUMBER specifies interval between prints.", required_argument, "float") \
  PARAM('P', "pcap-statistics", "Print pcap statistics every 5 seconds. The statistics do not behave the same way on all platforms.", no_argument, "none") \
  PARAM('m', "sample", "Sampling probability. NUMBER in 100 (DEFAULT: 100).", required_argument, "uint32") \
  PARAM('V', "vector", "Replacement vector. 1+32 NUMBERS.", required_argument, "string")

/**
 * \brief Parse input plugin settings.
 * \param [in] settings String containing input plugin settings.
 * \param [out] plugins Array for storing active plugins.
 * \param [in] module_options Options for plugin initialization.
 * \return Number of items specified in input string.
 */
int parse_plugin_settings(const string &settings, vector<FlowCachePlugin *> &plugins, options_t &module_options)
{
   string proto;
   size_t begin = 0, end = 0;

   int ifc_num = 0;
   while (end != string::npos) { // Iterate through user specified settings.
      end = settings.find(",", begin);
      proto = settings.substr(begin, (end == string::npos ? (settings.length() - begin) : (end - begin)));

      if (proto == "basic") {
         module_options.basic_ifc_num = ifc_num++; // Enable parsing basic flow (flow without any plugin output).
      } else if (proto == "http") {
         vector<plugin_opt> tmp;
         // Register extension header identifiers.
         // New configuration support sending plugin output to specific libtrap interface (e.g. http to ifc 1, dns to ifc 2...)
         // so it is necessary store extension-header -> output interface mapping within plugin.

         tmp.push_back(plugin_opt("http-req", http_request, ifc_num));
         tmp.push_back(plugin_opt("http-resp", http_response, ifc_num++));

         plugins.push_back(new HTTPPlugin(module_options, tmp));
      } else if (proto == "dns"){
         vector<plugin_opt> tmp;
         tmp.push_back(plugin_opt("dns", dns, ifc_num++));

         plugins.push_back(new DNSPlugin(module_options, tmp));
      } else if (proto == "sip"){
         vector<plugin_opt> tmp;
         tmp.push_back(plugin_opt("sip", sip, ifc_num++));

         plugins.push_back(new SIPPlugin(module_options, tmp));
      } else if (proto == "ntp"){
         vector<plugin_opt> tmp;
         tmp.push_back(plugin_opt("ntp", ntp, ifc_num++));

         plugins.push_back(new NTPPlugin(module_options, tmp));
      } else if (proto == "arp"){
         vector<plugin_opt> tmp;
         tmp.push_back(plugin_opt("arp", arp, ifc_num++));

         plugins.push_back(new ARPPlugin(module_options, tmp));
      } else {
         fprintf(stderr, "Unsupported plugin: \"%s\"\n", proto.c_str());
         return -1;
      }
      begin = end + 1;
   }

   return ifc_num;
}

/**
 * \brief Count ifc interfaces.
 * \param [in] argc Number of parameters.
 * \param [in] argv Pointer to parameters.
 * \return Number of ifc interfaces.
 */
int count_ifc_interfaces(int argc, char *argv[])
{
   char *interfaces = NULL;
   for (int i = 1; i < argc; i++) { // Find argument for param -i.
      if (!strcmp(argv[i], "-i") && i + 1 < argc) {
         interfaces = argv[i + 1];
      }
   }

   int int_cnt = 1;
   if (interfaces != NULL) {
      while(*interfaces) { // Count number of specified interfaces.
         if (*(interfaces++) == ',') {
            int_cnt++;
         }
      }
      return int_cnt;
   }

   return int_cnt;
}

/**
 * \brief Provides conversion from string to uint32_t.
 * \param [in] str String representation of value.
 * \param [out] dst Destination variable.
 * \return True on success, false otherwise.
 */
bool str_to_uint32(const char *str, uint32_t &dst)
{
   char *check;
   errno = 0;
   unsigned long long value = strtoull(str, &check, 10);
   if (errno == ERANGE || str[0] == '-' || *check ||
      value < numeric_limits<uint32_t>::min() ||
      value > numeric_limits<uint32_t>::max()) {
      return false;
   }

   dst = value;
   return true;
}

/**
 * \brief Provides conversion from string to double.
 * \param [in] str String representation of value.
 * \param [out] dst Destination variable.
 * \return True on success, false otherwise.
 */
bool str_to_double(const char *str, double &dst)
{
   char *check;
   errno = 0;
   double value = strtod(str, &check);
   if (errno == ERANGE || *check || str[0] == '\0') {
      return false;
   }

   dst = value;
   return true;
}

/**
 * \brief Convert double to struct timeval.
 * \param [in] value Value to convert.
 * \param [out] time Struct for storing converted time.
 */
static inline void double_to_timeval(double value, struct timeval &time)
{
   time.tv_sec = (long) value;
   time.tv_usec = (value - (long) value) * 1000000;
}

/**
 * \brief Exit and print an error message.
 * \param [in] e String containing an error message
 * \return EXIT_FAILURE
 */
inline bool error(const string &e)
{
   cerr << "flow_meter: " << e << endl;
   return EXIT_FAILURE;
}

/**
 * \brief Signal handler function.
 * \param [in] sig Signal number.
 */
void signal_handler(int sig)
{
   stop = 1;
}

int main(int argc, char *argv[])
{
   plugins_t plugin_wrapper;
   options_t options;
   options.flow_cache_size = DEFAULT_FLOW_CACHE_SIZE;
   options.flow_line_size = DEFAULT_FLOW_LINE_SIZE;
   double_to_timeval(DEFAULT_INACTIVE_TIMEOUT, options.inactive_timeout);
   double_to_timeval(DEFAULT_ACTIVE_TIMEOUT, options.active_timeout);
   options.replacement_string = DEFAULT_REPLACEMENT_STRING;
   options.print_stats = true; /* Plugins, FlowCache stats ON. */
   options.print_pcap_stats = false;
   options.interface = "";
   options.basic_ifc_num = 0;

   uint32_t pkt_limit = 0; // Limit of packets for packet parser. 0 = no limit
   int sampling = 100;
   srand(time(NULL));

   // ***** TRAP initialization *****
   INIT_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
   module_info->num_ifc_out = count_ifc_interfaces(argc, argv);
   TRAP_DEFAULT_INITIALIZATION(argc, argv, *module_info);

   signal(SIGTERM, signal_handler);
   signal(SIGINT, signal_handler);

   for (int i = 0; i < module_info->num_ifc_out; i++) {
      trap_ifcctl(TRAPIFC_OUTPUT, i, TRAPCTL_SETTIMEOUT, TRAP_WAIT);
   }

   signed char opt;
   while ((opt = TRAP_GETOPT(argc, argv, module_getopt_string, long_options)) != -1) {
      switch (opt) {
      case 'p':
         {
            options.basic_ifc_num = -1;
            int ret = parse_plugin_settings(string(optarg), plugin_wrapper.plugins, options);
            if (ret < 0) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -p");
            }
            if (ret != module_info->num_ifc_out) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Number of output ifc interfaces does not correspond number of items in -p parameter.");
            }
         }
         break;
      case 'c':
         {
            uint32_t tmp;
            if (!str_to_uint32(optarg, tmp)) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -c");
            }
            pkt_limit = tmp;
         }
         break;
      case 'I':
         options.interface = string(optarg);
         break;
      case 't':
         {
            if (!strcmp(optarg, "default")) {
               break;
            }

            char *check;
            check = strchr(optarg, ':');
            if (check == NULL) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -t");
            }

            *check = '\0';
            double tmp1, tmp2;
            if (!str_to_double(optarg, tmp1) || !str_to_double(check + 1, tmp2) || tmp1 < 0 || tmp2 < 0) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -t");
            }

            double_to_timeval(tmp1, options.active_timeout);
            double_to_timeval(tmp2, options.inactive_timeout);
         }
         break;
      case 'r':
         options.pcap_file = string(optarg);
         break;
      case 's':
         if (strcmp(optarg, "default")) {
            uint32_t tmp;
            if (!str_to_uint32(optarg, tmp) || tmp == 0) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -s");
            }
            options.flow_cache_size = tmp;
         } else {
            options.flow_cache_size = DEFAULT_FLOW_CACHE_SIZE;
         }
         break;
      case 'S':
         {
            double tmp;
            if (!str_to_double(optarg, tmp)) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -s");
            }
            double_to_timeval(tmp, options.cache_stats_interval);
            options.print_stats = false; /* Plugins, FlowCache stats OFF.*/
         }
         break;
      case 'P':
         options.print_pcap_stats = true;
         break;
      case 'm':
         {
            uint32_t tmp;
            if (!str_to_uint32(optarg, tmp)) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -s");
            }

            sampling = tmp;
            if (sampling < 0 || sampling > 100) {
               FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
               TRAP_DEFAULT_FINALIZATION();
               return error("Invalid argument for option -m: interval needs to be between 0-100");
            }
         }
         break;
      case 'V':
         options.replacement_string = optarg;
         break;
      default:
         FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
         TRAP_DEFAULT_FINALIZATION();
         return error("Invalid arguments");
      }
   }

   if (options.interface != "" && options.pcap_file != "") {
      FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
      TRAP_DEFAULT_FINALIZATION();
      return error("Cannot capture from file and from interface at the same time.");
   } else if (options.interface == "" && options.pcap_file == "") {
      FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
      TRAP_DEFAULT_FINALIZATION();
      return error("Specify capture interface (-I) or file for reading (-r). ");
   }

   if (options.flow_cache_size % options.flow_line_size != 0) {
      FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
      TRAP_DEFAULT_FINALIZATION();
      return error("Size of flow line (32 by default) must divide size of flow cache.");
   }

   PcapReader packetloader(options);
   if (options.interface == "") {
      if (packetloader.open_file(options.pcap_file) != 0) {
         FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
         TRAP_DEFAULT_FINALIZATION();
         return error("Can't open input file: " + options.pcap_file);
      }
   } else {
      if (packetloader.init_interface(options.interface) != 0) {
         FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
         TRAP_DEFAULT_FINALIZATION();
         return error("Unable to initialize libpcap: " + packetloader.error_msg);
      }
   }

   NHTFlowCache flowcache(options);
   UnirecExporter flowwriter;

   if (flowwriter.init(plugin_wrapper.plugins, module_info->num_ifc_out, options.basic_ifc_num) != 0) {
      FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
      TRAP_DEFAULT_FINALIZATION();
      return error("Unable to initialize UnirecExporter.");
   }
   flowcache.set_exporter(&flowwriter);

   if (!options.print_stats) {
      plugin_wrapper.plugins.push_back(new StatsPlugin(options.cache_stats_interval, cout));
   }

   for (unsigned int i = 0; i < plugin_wrapper.plugins.size(); i++) {
      flowcache.add_plugin(plugin_wrapper.plugins[i]);
   }

   flowcache.init();

   Packet packet;
   int ret = 0;
   uint32_t pkt_total = 0, pkt_parsed = 0;
   packet.packet = new char[MAXPCKTSIZE + 1];

   /* Main packet capture loop. */
   while (!stop && (ret = packetloader.get_pkt(packet)) > 0) {
      if (ret == 3) { /* Process timeout. */
         flowcache.export_expired(false);
         continue;
      }

      pkt_total++;
      if (ret == 2 && (sampling == 100 || ((rand() % 100) + 1) <= sampling)) {
         flowcache.put_pkt(packet);
         pkt_parsed++;

         /* Check if packet limit is reached. */
         if (pkt_limit != 0 && pkt_parsed >= pkt_limit) {
            break;
         }
      }
   }

   if (ret < 0) {
      packetloader.close();
      flowwriter.close();
      delete [] packet.packet;
      FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
      TRAP_DEFAULT_FINALIZATION();
      return error("Error during reading: " + packetloader.error_msg);
   }

   if (options.print_stats) {
      cout << "Total packets processed: "<< pkt_total << endl;
      cout << "Packet headers parsed: "<< pkt_parsed << endl;
   }

   /* Cleanup. */
   flowcache.finish();
   flowwriter.close();
   packetloader.close();

   delete [] packet.packet;
   FREE_MODULE_INFO_STRUCT(MODULE_BASIC_INFO, MODULE_PARAMS);
   TRAP_DEFAULT_FINALIZATION();

   return EXIT_SUCCESS;
}
