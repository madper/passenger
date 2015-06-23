/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010-2015 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_UST_ROUTER_OPTION_PARSER_H_
#define _PASSENGER_UST_ROUTER_OPTION_PARSER_H_

#include <cstdio>
#include <cstdlib>
#include <Constants.h>
#include <Utils.h>
#include <Utils/VariantMap.h>
#include <Utils/OptionParsing.h>
#include <Utils/StrIntUtils.h>

namespace Passenger {

using namespace std;


inline void
ustRouterUsage() {
	printf("Usage: " AGENT_EXE " ust-router <OPTIONS...>\n");
	printf("Runs the " PROGRAM_NAME " UstRouter.\n");
	printf("\n");
	printf("Required options:\n");
	printf("      --passenger-root PATH   The location to the " PROGRAM_NAME " source\n");
	printf("                              directory\n");
	printf("      --password-file PATH    Protect the UstRouter controller with the password in\n");
	printf("                              this file\n");
	printf("\n");
	printf("Other options (optional):\n");
	printf("  -l, --listen ADDRESS        Listen on the given address. The address must be\n");
	printf("                              formatted as tcp://IP:PORT for TCP sockets, or\n");
	printf("                              unix:PATH for Unix domain sockets.\n");
	printf("                              " DEFAULT_UST_ROUTER_LISTEN_ADDRESS "\n");
	printf("\n");
	printf("      --api-listen ADDRESS    Listen on the given address for API commands.\n");
	printf("                              The address must be in the same format as that\n");
	printf("                              of --listen. Default: " DEFAULT_UST_ROUTER_API_LISTEN_ADDRESS "\n");
	printf("      --authorize [LEVEL]:USERNAME:PASSWORDFILE\n");
	printf("                              Enables authentication on the API server,\n");
	printf("                              through the given API account. LEVEL indicates\n");
	printf("                              the privilege level (see below). PASSWORDFILE must\n");
	printf("                              point to a file containing the password\n");
	printf("\n");
	printf("      --dump-file PATH        Dump transactions without Union Station key to the\n");
	printf("                              following file. Default: /dev/null\n");
	printf("\n");
	printf("      --user USERNAME         Lower privilege to the given user. Only has\n");
	printf("                              effect when started as root\n");
	printf("      --group GROUPNAME       Lower privilege to the given group. Only has\n");
	printf("                              effect when started as root. Default: primary\n");
	printf("                              group of the username given by '--user'\n");
	printf("\n");
	printf("      --log-file PATH         Log to the given file.\n");
	printf("      --log-level LEVEL       Logging level. Default: %d\n", DEFAULT_LOG_LEVEL);
	printf("\n");
	printf("  -h, --help                  Show this help\n");
	printf("\n");
	printf("API account privilege levels (ordered from most to least privileges):\n");
	printf("  readonly    Read-only access\n");
	printf("  full        Full access (default)\n");
}

inline bool
parseUstRouterOption(int argc, const char *argv[], int &i, VariantMap &options) {
	OptionParser p(ustRouterUsage);

	if (p.isValueFlag(argc, i, argv[i], '\0', "--passenger-root")) {
		options.set("passenger_root", argv[i + 1]);
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--password-file")) {
		options.set("ust_router_password_file", argv[i + 1]);
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], 'l', "--listen")) {
		if (getSocketAddressType(argv[i + 1]) != SAT_UNKNOWN) {
			options.set("ust_router_address", argv[i + 1]);
			i += 2;
		} else {
			fprintf(stderr, "ERROR: invalid address format for --listen. The address "
				"must be formatted as tcp://IP:PORT for TCP sockets, or unix:PATH "
				"for Unix domain sockets.\n");
			exit(1);
		}
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--api-listen")) {
		if (getSocketAddressType(argv[i + 1]) != SAT_UNKNOWN) {
			vector<string> addresses = options.getStrSet("ust_router_api_addresses",
				false);
			if (addresses.size() == SERVER_KIT_MAX_SERVER_ENDPOINTS) {
				fprintf(stderr, "ERROR: you may specify up to %u --api-listen addresses.\n",
					SERVER_KIT_MAX_SERVER_ENDPOINTS);
				exit(1);
			}
			addresses.push_back(argv[i + 1]);
			options.setStrSet("ust_router_api_addresses", addresses);
			i += 2;
		} else {
			fprintf(stderr, "ERROR: invalid address format for --api-listen. The address "
				"must be formatted as tcp://IP:PORT for TCP sockets, or unix:PATH "
				"for Unix domain sockets.\n");
			exit(1);
		}
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--authorize")) {
		vector<string> args;
		vector<string> authorizations = options.getStrSet("ust_router_authorizations",
				false);

		split(argv[i + 1], ':', args);
		if (args.size() < 2 || args.size() > 3) {
			fprintf(stderr, "ERROR: invalid format for --authorize. The syntax "
				"is \"[LEVEL:]USERNAME:PASSWORDFILE\".\n");
			exit(1);
		}

		authorizations.push_back(argv[i + 1]);
		options.setStrSet("ust_router_authorizations", authorizations);
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--dump-file")) {
		options.set("ust_router_dump_file", argv[i + 1]);
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--user")) {
		options.set("analytics_log_user", argv[i + 1]);
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--group")) {
		options.set("analytics_log_group", argv[i + 1]);
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--log-level")) {
		// We do not set log_level because, when this function is called from
		// the Watchdog, we don't want to affect the Watchdog's own log level.
		options.setInt("ust_router_log_level", atoi(argv[i + 1]));
		i += 2;
	} else if (p.isValueFlag(argc, i, argv[i], '\0', "--log-file")) {
		// We do not set debug_log_file because, when this function is called from
		// the Watchdog, we don't want to affect the Watchdog's own log file.
		options.set("ust_router_log_file", argv[i + 1]);
		i += 2;
	} else {
		return false;
	}
	return true;
}


} // namespace Passenger

#endif /* _PASSENGER_UST_ROUTER_OPTION_PARSER_H_ */