/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2012-2017 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */

/*
 * Sets given environment variables, dumps the entire environment to
 * a given file (for diagnostics purposes), then execs the given command.
 *
 * This is a separate executable because it does quite
 * some non-async-signal-safe stuff that we can't do after
 * fork()ing from the Spawner and before exec()ing.
 */

#include <oxt/initialize.hpp>
#include <oxt/backtrace.hpp>
#include <boost/scoped_array.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>

#include <jsoncpp/json.h>
#include <adhoc_lve.h>

#include <Logging.h>
#include <Utils.h>
#include <Utils/IOUtils.h>
#include <Utils/StrIntUtils.h>
#include <Core/SpawningKit/Exceptions.h>

using namespace std;
using namespace Passenger;

extern "C" {
	extern char **environ;
}


static Json::Value
readArgsJson(const string &workDir) {
	Json::Reader reader;
	Json::Value result;
	string contents = readAll(workDir + "/args.json");
	if (reader.parse(contents, result)) {
		return result;
	} else {
		P_CRITICAL("Cannot parse " << workDir << "/args.json: "
			<< reader.getFormattedErrorMessages());
		exit(1);
		// Never reached
		return Json::Value();
	}
}

static void
initializeLogLevel(const Json::Value &args) {
	if (args.isMember("log_level")) {
		setLogLevel(args["log_level"].asInt());
	}
}

static void
setAndPrintCurrentErrorSummaryHTML(const string &workDir, const string &message) {
	fprintf(stderr, "%s\n", message.c_str());

	string path = workDir + "/response/error_summary";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: unable to create %s: %s\n",
			path.c_str(), e.what());
	}
}

static void
setCurrentErrorSource(const string &workDir, SpawningKit::SpawnException::ErrorSource errorSource) {
	string path = workDir + "/response/error_source";
	try {
		createFile(path.c_str(), SpawningKit::SpawnException::errorSourceToString(errorSource));
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: unable to create %s: %s\n",
			path.c_str(), e.what());
	}
}

static void
setCurrentErrorKind(const string &workDir, SpawningKit::SpawnException::ErrorKind errorKind) {
	string path = workDir + "/response/error_kind";
	try {
		createFile(path.c_str(), SpawningKit::SpawnException::errorKindToString(errorKind));
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: unable to create %s: %s\n",
			path.c_str(), e.what());
	}
}

static void
setProblemDescriptionHTML(const string &workDir, const string &message) {
	string path = workDir + "/response/problem_description.html";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: unable to create %s: %s\n",
			path.c_str(), e.what());
	}
}

static void
setSolutionDescriptionHTML(const string &workDir, const string &message) {
	string path = workDir + "/response/solution_description.html";
	try {
		createFile(path.c_str(), message);
	} catch (const FileSystemException &e) {
		fprintf(stderr, "Warning: unable to create %s: %s\n",
			path.c_str(), e.what());
	}
}

static void
dumpEnvvars(const string &workDir) {
	FILE *f = fopen((workDir + "/response/envvars").c_str(), "w");
	if (f != NULL) {
		unsigned int i = 0;
		while (environ[i] != NULL) {
			fputs(environ[i], f);
			putc('\n', f);
			i++;
		}
		fclose(f);
	}
}

static void
dumpUserInfo(const string &workDir) {
	FILE *f = fopen((workDir + "/user_info").c_str(), "w");
	if (f != NULL) {
		pid_t pid = fork();
		if (pid == 0) {
			dup2(fileno(f), 1);
			execlp("id", "id", (char *) 0);
			_exit(1);
		} else if (pid == -1) {
			int e = errno;
			fprintf(stderr, "Error: cannot fork a new process: %s (errno=%d)\n",
				strerror(e), e);
		} else {
			waitpid(pid, NULL, 0);
		}
		fclose(f);
	}
}

static void
dumpUlimits(const string &workDir) {
	FILE *f = fopen((workDir + "/ulimit").c_str(), "w");
	if (f != NULL) {
		pid_t pid = fork();
		if (pid == 0) {
			dup2(fileno(f), 1);
			execlp("ulimit", "ulimit", "-a", (char *) 0);
			_exit(1);
		} else if (pid == -1) {
			int e = errno;
			fprintf(stderr, "Error: cannot fork a new process: %s (errno=%d)\n",
			strerror(e), e);
		} else {
			waitpid(pid, NULL, 0);
		}
		fclose(f);
	}
}

static void
dumpAllEnvironmentInfo(const string &workDir) {
	dumpEnvvars(workDir);
	dumpUserInfo(workDir);
	dumpUlimits(workDir);
}

static bool
setUlimits(const Json::Value &args) {
	if (!args.isMember("file_descriptor_ulimit")) {
		return false;
	}

	rlim_t fdLimit = (rlim_t) args["file_descriptor_ulimit"].asUInt();
	struct rlimit limit;
	int ret;

	limit.rlim_cur = fdLimit;
	limit.rlim_max = fdLimit;
	do {
		ret = setrlimit(RLIMIT_NOFILE, &limit);
	} while (ret == -1 && errno == EINTR);

	if (ret == -1) {
		int e = errno;
		fprintf(stderr, "Error: unable to set file descriptor ulimit to %u: %s (errno=%d)",
			(unsigned int) fdLimit, strerror(e), e);
	}

	return ret != -1;
}

static bool
canSwitchUser(const Json::Value &args) {
	return args.isMember("user") && geteuid() == 0;
}

static void
lookupUserGroup(const string &workDir, const Json::Value &args, uid_t *uid,
	struct passwd **userInfo, gid_t *gid)
{
	errno = 0;
	*userInfo = getpwnam(args["user"].asCString());
	if (*userInfo == NULL) {
		int e = errno;
		if (looksLikePositiveNumber(args["user"].asString())) {
			fprintf(stderr,
				"Warning: error looking up system user database entry for user '%s': %s (errno=%d)\n",
				args["user"].asCString(), strerror(e), e);
			*uid = (uid_t) atoi(args["user"].asString());
		} else {
			setCurrentErrorKind(workDir,
				SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
			setAndPrintCurrentErrorSummaryHTML(workDir,
				"Cannot lookup up system user database entry for user '"
				+ args["user"].asString() + "': " + strerror(e)
				+ " (errno=" + toString(e) + ")");
			exit(1);
		}
	} else {
		*uid = (*userInfo)->pw_uid;
	}

	errno = 0;
	struct group *groupInfo = getgrnam(args["group"].asCString());
	if (groupInfo == NULL) {
		int e = errno;
		if (looksLikePositiveNumber(args["group"].asString())) {
			fprintf(stderr,
				"Warning: error looking up system group database entry for group '%s': %s (errno=%d)\n",
				args["group"].asCString(), strerror(e), e);
			*gid = (gid_t) atoi(args["group"].asString());
		} else {
			setCurrentErrorKind(workDir,
				SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
			setAndPrintCurrentErrorSummaryHTML(workDir,
				"Cannot lookup up system group database entry for group '"
				+ args["group"].asString() + "': " + strerror(e)
				+ " (errno=" + toString(e) + ")");
			exit(1);
		}
	} else {
		*gid = groupInfo->gr_gid;
	}
}

static void
enterLveJail(const string &workDir, const struct passwd *userInfo) {
	string lveInitErr;
	adhoc_lve::LibLve &liblve = adhoc_lve::LveInitSignleton::getInstance(&lveInitErr);

	if (liblve.is_error()) {
		if (!lveInitErr.empty()) {
			lveInitErr = ": " + lveInitErr;
		}
		setCurrentErrorKind(workDir,
			SpawningKit::SpawnException::INTERNAL_ERROR);
		setAndPrintCurrentErrorSummaryHTML(workDir,
			"Failed to initialize LVE library: " + lveInitErr);
		exit(1);
	}

	if (!liblve.is_lve_available()) {
		return;
	}

	string jailErr;
	int ret = liblve.jail(userInfo, jailErr);
	if (ret < 0) {
		setCurrentErrorKind(workDir,
			SpawningKit::SpawnException::INTERNAL_ERROR);
		setAndPrintCurrentErrorSummaryHTML(workDir,
			"enterLve() failed: " + jailErr);
		exit(1);
	}
}

static void
switchGroup(const string &workDir, uid_t uid, const struct passwd *userInfo, gid_t gid) {
	if (userInfo != NULL) {
		bool setgroupsCalled = false;

		#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
			#ifdef __APPLE__
				int groups[1024];
				int ngroups = sizeof(groups) / sizeof(int);
			#else
				gid_t groups[1024];
				int ngroups = sizeof(groups) / sizeof(gid_t);
			#endif
			boost::scoped_array<gid_t> gidset;

			int ret = getgrouplist(userInfo->pw_name, gid,
				groups, &ngroups);
			if (ret == -1) {
				int e = errno;
				setCurrentErrorKind(workDir,
					SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
				setAndPrintCurrentErrorSummaryHTML(workDir,
					"Error: getgrouplist(" + string(userInfo->pw_name) + ", "
					+ toString(gid) + ") failed: " + strerror(e)
					+ " (errno=" + toString(e) + ")");
				exit(1);
			}

			if (ngroups <= NGROUPS_MAX) {
				setgroupsCalled = true;
				gidset.reset(new gid_t[ngroups]);
				if (setgroups(ngroups, gidset.get()) == -1) {
					int e = errno;
					setCurrentErrorKind(workDir,
						SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
					setAndPrintCurrentErrorSummaryHTML(workDir,
						"Error: setgroups(" + toString(ngroups)
						+ ", ...) failed: " + strerror(e) + " (errno="
						+ toString(e) + ")");
					exit(1);
				}
			}
		#endif

		if (!setgroupsCalled && initgroups(userInfo->pw_name, gid) == -1) {
			int e = errno;
			setCurrentErrorKind(workDir,
				SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
			setAndPrintCurrentErrorSummaryHTML(workDir,
				"Error: initgroups(" + string(userInfo->pw_name)
				+ ", " + toString(gid) + ") failed: " + strerror(e)
				+ " (errno=" + toString(e) + ")");
			exit(1);
		}
	}

	if (setgid(gid) == -1) {
		int e = errno;
		setCurrentErrorKind(workDir,
			SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
		setAndPrintCurrentErrorSummaryHTML(workDir,
			"Error: setgid(" + toString(gid) + ") failed: "
			+ strerror(e) + " (errno=" + toString(e) + ")");
		exit(1);
	}
}

static void
switchUser(const string &workDir, uid_t uid, const struct passwd *userInfo) {
	if (setuid(uid) == -1) {
		int e = errno;
		setCurrentErrorKind(workDir,
			SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
		setAndPrintCurrentErrorSummaryHTML(workDir,
			"Error: setuid(" + toString(uid) + ") failed: " + strerror(e)
			+ " (errno=" + toString(e) + ")");
		exit(1);
	}
	if (userInfo != NULL) {
		setenv("USER", userInfo->pw_name, 1);
		setenv("LOGNAME", userInfo->pw_name, 1);
		setenv("SHELL", userInfo->pw_shell, 1);
		setenv("HOME", userInfo->pw_dir, 1);
	} else {
		unsetenv("USER");
		unsetenv("LOGNAME");
		unsetenv("SHELL");
		unsetenv("HOME");
	}
}

static string
lookupCurrentUserShell() {
	struct passwd *userInfo = getpwuid(getuid());
	if (userInfo == NULL) {
		int e = errno;
		fprintf(stderr, "Warning: cannot lookup system user database"
			" entry for UID %d: %s (errno=%d)\n",
			(int) getuid(), strerror(e), e);
		return "/bin/sh";
	} else {
		return userInfo->pw_shell;
	}
}

static vector<string>
inferAllParentDirectories(const string &path) {
	vector<string> components, result;

	split(path, '/', components);
	P_ASSERT_EQ(components.front(), "");
	components.erase(components.begin());

	for (unsigned int i = 0; i < components.size(); i++) {
		string path2;
		for (unsigned int j = 0; j <= i; j++) {
			path2.append("/");
			path2.append(components[j]);
		}
		if (path2.empty()) {
			path2 = "/";
		}
		result.push_back(path2);
	}

	P_ASSERT_EQ(result.back(), path);
	return result;
}

static void
setCurrentWorkingDirectory(const string &workDir, const Json::Value &args) {
	string appRoot = absolutizePath(args["app_root"].asString());
	vector<string> appRootAndParentDirs = inferAllParentDirectories(appRoot);
	vector<string>::const_iterator it;
	int ret;

	for (it = appRootAndParentDirs.begin(); it != appRootAndParentDirs.end(); it++) {
		struct stat buf;
		ret = stat(it->c_str(), &buf);
		if (ret == -1 && errno == EACCES) {
			char parent[PATH_MAX];
			const char *end = strrchr(it->c_str(), '/');
			memcpy(parent, it->c_str(), end - it->c_str());
			parent[end - it->c_str()] = '\0';

			setCurrentErrorKind(workDir,
				SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
			setAndPrintCurrentErrorSummaryHTML(workDir,
				"Directory '" + string(parent) + "' is inaccessible because of a"
				" filesystem permission error.");
			setProblemDescriptionHTML(workDir,
				"The " PROGRAM_NAME " application server tried to start the"
				" web application as user '" + getProcessUsername() + "' and group '"
				+ getGroupName(getgid()) + "'. During this process, "
				SHORT_PROGRAM_NAME " must be able to access its application"
				" root directory '" + appRoot + "'. However, the parent directory '"
				+ parent + "' has wrong permissions, thereby preventing this"
				" process from accessing its application root directory.");
			setSolutionDescriptionHTML(workDir,
				"Please fix the permissions of the directory '" + appRoot
				+ "' in such a way that the directory is accessible by user '"
				+ getProcessUsername() + "' and group '"
				+ getGroupName(getgid()) + "'.");
			exit(1);
		} else if (ret == -1) {
			int e = errno;
			setCurrentErrorKind(workDir,
				SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
			setAndPrintCurrentErrorSummaryHTML(workDir,
				"Unable to stat() directory '" + *it + "': "
				+ strerror(e) + " (errno=" + toString(e) + ")");
			exit(1);
		}
	}

	ret = chdir(appRoot.c_str());
	if (ret != 0) {
		int e = errno;
		setCurrentErrorKind(workDir,
			SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
		setAndPrintCurrentErrorSummaryHTML(workDir,
			"Unable to change working directory to '" + appRoot + "': "
			+ strerror(e) + " (errno=" + toString(e) + ")");
		if (e == EPERM || e == EACCES) {
			setProblemDescriptionHTML(workDir,
				"<p>The " PROGRAM_NAME " application server tried to start the"
				" web application as user " + getProcessUsername() + " and group "
				+ getGroupName(getgid()) + ", with a working directory of "
				+ appRoot.c_str() + ". However, it encountered a filesystem"
				" permission error while doing this.</p>");
		} else {
			setProblemDescriptionHTML(workDir,
				"<p>The " PROGRAM_NAME " application server tried to start the"
				" web application as user " + getProcessUsername() + " and group "
				+ getGroupName(getgid()) + ", with a working directory of "
				+ appRoot.c_str() + ". However, it encountered a filesystem"
				" error while doing this.</p>");
		}
		exit(1);
	}

	// The application root may contain one or more symlinks
	// in its path. If the application calls getcwd(), it will
	// get the resolved path.
	//
	// It turns out that there is no such thing as a path without
	// unresolved symlinks. The shell presents a working directory with
	// unresolved symlinks (which it calls the "logical working directory"),
	// but that is an illusion provided by the shell. The shell reports
	// the logical working directory though the PWD environment variable.
	//
	// See also:
	// https://github.com/phusion/passenger/issues/1596#issuecomment-138154045
	// http://git.savannah.gnu.org/cgit/coreutils.git/tree/src/pwd.c
	// http://www.opensource.apple.com/source/shell_cmds/shell_cmds-170/pwd/pwd.c
	setenv("PWD", appRoot.c_str(), 1);
}

static void
setDefaultEnvvars(const Json::Value &args) {
	setenv("PYTHONUNBUFFERED", "1", 1);

	setenv("NODE_PATH", args["node_libdir"].asCString(), 1);

	setenv("RAILS_ENV", args["app_env"].asCString(), 1);
	setenv("RACK_ENV", args["app_env"].asCString(), 1);
	setenv("WSGI_ENV", args["app_env"].asCString(), 1);
	setenv("NODE_ENV", args["app_env"].asCString(), 1);
	setenv("PASSENGER_APP_ENV", args["app_env"].asCString(), 1);

	if (args.isMember("expected_start_port")) {
		setenv("PORT", toString(args["expected_start_port"].asInt()).c_str(), 1);
	}

	if (args["base_uri"].asString() != "/") {
		setenv("RAILS_RELATIVE_URL_ROOT", args["base_uri"].asCString(), 1);
		setenv("RACK_BASE_URI", args["base_uri"].asCString(), 1);
		setenv("PASSENGER_BASE_URI", args["base_uri"].asCString(), 1);
	} else {
		unsetenv("RAILS_RELATIVE_URL_ROOT");
		unsetenv("RACK_BASE_URI");
		unsetenv("PASSENGER_BASE_URI");
	}
}

static void
setGivenEnvVars(const Json::Value &args) {
	const Json::Value &envvars = args["environment_variables"];
	Json::Value::const_iterator it, end = envvars.end();

	for (it = envvars.begin(); it != end; it++) {
		const char *key = it.memberName();
		setenv(key, (*it).asCString(), 1);
	}
}

static bool
shouldLoadShellEnvvars(const Json::Value &args, const string &shell) {
	if (args["load_shell_envvars"].asBool()) {
		string shellName = extractBaseName(shell);
		return shellName == "bash" || shellName == "zsh" || shellName == "ksh";
	} else {
		return false;
	}
}

static string
commandArgsToString(const vector<const char *> &commandArgs) {
	vector<const char *>::const_iterator it;
	string result;

	for (it = commandArgs.begin(); it != commandArgs.end(); it++) {
		if (*it != NULL) {
			result.append(*it);
			result.append(1, ' ');
		}
	}

	return strip(result);
}

static void
execNextCommand(const string &workDir, const Json::Value &args,
	const char *mode, const string &shell)
{
	vector<const char *> commandArgs;

	// Note: do not try to set a process title in this function by messing with argv[0].
	// https://code.google.com/p/phusion-passenger/issues/detail?id=855

	if (strcmp(mode, "--before") == 0) {
		assert(!shell.empty());
		if (shouldLoadShellEnvvars(args, shell)) {
			setCurrentErrorSource(workDir,
				SpawningKit::SpawnException::OS_SHELL);
			commandArgs.push_back(shell.c_str());
			commandArgs.push_back("-lc");
			commandArgs.push_back("exec \"$@\"");
			commandArgs.push_back("SpawnEnvSetupperShell");
		} else {
			setCurrentErrorSource(workDir,
				SpawningKit::SpawnException::SPAWN_ENV_SETUPPER_AFTER_SHELL);
		}
		commandArgs.push_back(args["passenger_agent_path"].asCString());
		commandArgs.push_back("spawn-env-setupper");
		commandArgs.push_back(workDir.c_str());
		commandArgs.push_back("--after");
	} else {
		if (args["starts_using_wrapper"].asBool()) {
			setCurrentErrorSource(workDir, SpawningKit::SpawnException::WRAPPER);
		} else {
			setCurrentErrorSource(workDir, SpawningKit::SpawnException::APP);
		}
		commandArgs.push_back("/bin/sh");
		commandArgs.push_back("-c");
		commandArgs.push_back(args["start_command"].asCString());
	}
	commandArgs.push_back(NULL);

	execvp(commandArgs[0], (char * const *) &commandArgs[0]);

	int e = errno;
	setCurrentErrorKind(workDir,
		SpawningKit::SpawnException::OPERATING_SYSTEM_ERROR);
	if (strcmp(mode, "--before") == 0) {
		setCurrentErrorSource(workDir,
			SpawningKit::SpawnException::SPAWN_ENV_SETUPPER_BEFORE_SHELL);
	} else {
		setCurrentErrorSource(workDir,
			SpawningKit::SpawnException::SPAWN_ENV_SETUPPER_AFTER_SHELL);
	}
	setAndPrintCurrentErrorSummaryHTML(workDir,
		"Unable to execute command '" + commandArgsToString(commandArgs)
		+ "': " + strerror(e) + " (errno=" + toString(e) + ")");
	exit(1);
}

int
spawnEnvSetupperMain(int argc, char *argv[]) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (argc != 4) {
		fprintf(stderr, "Usage: PassengerAgent spawn-env-setupper <workdir> <--before|--after>\n");
		exit(1);
	}

	string workDir = argv[2];
	setenv("IN_PASSENGER", "1", 1);
	setenv("PASSENGER_SPAWN_WORK_DIR", workDir.c_str(), 1);
	dumpAllEnvironmentInfo(workDir);

	try {
		oxt::initialize();
		oxt::setup_syscall_interruption_support();

		Json::Value args = readArgsJson(workDir);
		const char *mode = argv[3];
		bool shouldTrySwitchUser = canSwitchUser(args);
		string shell;

		initializeLogLevel(args);

		if (strcmp(mode, "--before") == 0) {
			struct passwd *userInfo = NULL;
			uid_t uid;
			gid_t gid;

			setCurrentErrorSource(workDir,
				SpawningKit::SpawnException::SPAWN_ENV_SETUPPER_BEFORE_SHELL);
			setDefaultEnvvars(args);
			dumpEnvvars(workDir);

			if (shouldTrySwitchUser) {
				lookupUserGroup(workDir, args, &uid, &userInfo, &gid);
				shell = userInfo->pw_shell;
			} else {
				shell = lookupCurrentUserShell();
			}
			if (setUlimits(args)) {
				dumpUlimits(workDir);
			}
			if (shouldTrySwitchUser) {
				enterLveJail(workDir, userInfo);
				switchGroup(workDir, uid, userInfo, gid);
				dumpUserInfo(workDir);

				switchUser(workDir, uid, userInfo);
				dumpEnvvars(workDir);
				dumpUserInfo(workDir);
			}
		} else {
			setCurrentErrorSource(workDir,
				SpawningKit::SpawnException::SPAWN_ENV_SETUPPER_AFTER_SHELL);
		}

		setCurrentWorkingDirectory(workDir, args);
		dumpEnvvars(workDir);

		if (strcmp(mode, "--after") == 0) {
			setDefaultEnvvars(args);
			setGivenEnvVars(args);
			dumpEnvvars(workDir);
		}

		execNextCommand(workDir, args, mode, shell);
	} catch (const oxt::tracable_exception &e) {
		fprintf(stderr, "Error: %s\n%s\n",
			e.what(), e.backtrace().c_str());
		return 1;
	} catch (const std::exception &e) {
		fprintf(stderr, "Error: %s\n", e.what());
		return 1;
	}

	// Should never be reached
	fprintf(stderr, "*** BUG IN SpawnEnvSetupper ***: end of main() reached");
	return 1;
}
