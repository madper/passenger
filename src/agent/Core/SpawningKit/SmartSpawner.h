/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2017 Phusion Holding B.V.
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
#ifndef _PASSENGER_SPAWNING_KIT_SMART_SPAWNER_H_
#define _PASSENGER_SPAWNING_KIT_SMART_SPAWNER_H_

#include <oxt/thread.hpp>
#include <oxt/system_calls.hpp>
#include <boost/bind.hpp>
#include <boost/make_shared.hpp>
#include <string>
#include <vector>
#include <map>
#include <cassert>

#include <adhoc_lve.h>

#include <Logging.h>
#include <Constants.h>
#include <Utils/SystemTime.h>
#include <Utils/IOUtils.h>
#include <Utils/BufferedIO.h>
#include <Utils/JsonUtils.h>
#include <Utils/ScopeGuard.h>
#include <LveLoggingDecorator.h>
#include <Core/SpawningKit/Spawner.h>
#include <Core/SpawningKit/Exceptions.h>
#include <Core/SpawningKit/PipeWatcher.h>
#include <Core/SpawningKit/Handshake/BackgroundIOCapturer.h.h>

namespace Passenger {
namespace SpawningKit {

using namespace std;
using namespace boost;
using namespace oxt;


class SmartSpawner: public Spawner {
private:
	/**
	 * Structure containing arguments and working state for negotiating
	 * the preloader startup protocol.
	 */
	struct StartupDetails {
		/****** Arguments ******/
		pid_t pid;
		FileDescriptor adminSocket;
		BufferedIO io;
		BackgroundIOCapturerPtr stderrCapturer;
		WorkDirPtr workDir;
		const Options *options;

		/****** Working state ******/
		unsigned long long timeout;

		StartupDetails() {
			options = NULL;
			timeout = 0;
		}
	};

	const vector<string> preloaderCommand;
	map<string, string> preloaderAnnotations;
	AppPoolOptions options;

	// Protects m_lastUsed and pid.
	mutable boost::mutex simpleFieldSyncher;
	// Protects everything else.
	mutable boost::mutex syncher;

	// Preloader information.
	pid_t pid;
	FileDescriptor adminSocket;
	string socketAddress;
	unsigned long long m_lastUsed;
	// Upon starting the preloader, its preparation info is stored here
	// for future reference.
	SpawnPreparationInfo preparation;

	string getPreloaderCommandString() const {
		string result;
		unsigned int i;

		for (i = 0; i < preloaderCommand.size(); i++) {
			if (i != 0) {
				result.append(1, '\0');
			}
			result.append(preloaderCommand[i]);
		}
		return result;
	}

	vector<string> createRealPreloaderCommand(const Options &options,
		shared_array<const char *> &args)
	{
		string agentFilename = config->resourceLocator->findSupportBinary(AGENT_EXE);
		vector<string> command;

		if (shouldLoadShellEnvvars(options, preparation)) {
			command.push_back(preparation.userSwitching.shell);
			command.push_back(preparation.userSwitching.shell);
			command.push_back("-lc");
			command.push_back("exec \"$@\"");
			command.push_back("SpawnPreparerShell");
		} else {
			command.push_back(agentFilename);
		}
		command.push_back(agentFilename);
		command.push_back("spawn-preparer");
		command.push_back(preparation.appRoot);
		command.push_back(serializeEnvvarsFromPoolOptions(options));
		command.push_back(preloaderCommand[0]);
		// Note: do not try to set a process title here.
		// https://code.google.com/p/phusion-passenger/issues/detail?id=855
		command.push_back(preloaderCommand[0]);
		for (unsigned int i = 1; i < preloaderCommand.size(); i++) {
			command.push_back(preloaderCommand[i]);
		}

		createCommandArgs(command, args);
		return command;
	}

	void setConfigFromAppPoolOptions(Config *config, Json::Value &extraArgs,
		const AppPoolOptions &options)
	{
		Spawner::setConfigFromAppPoolOptions(config, extraArgs, options);
		config->spawnMethod = P_STATIC_STRING("smart");
	}

	void throwPreloaderSpawnException(const string &msg,
		SpawnException::ErrorKind errorKind,
		StartupDetails &details)
	{
		throwPreloaderSpawnException(msg, errorKind, details.stderrCapturer,
			*details.options, details.workDir);
	}

	void throwPreloaderSpawnException(const string &msg,
		SpawnException::ErrorKind errorKind,
		BackgroundIOCapturerPtr &stderrCapturer,
		const Options &options,
		const WorkDirPtr &workDir)
	{
		TRACE_POINT();
		// Stop the stderr capturing thread and get the captured stderr
		// output so far.
		string stderrOutput;
		if (stderrCapturer != NULL) {
			stderrOutput = stderrCapturer->stop();
		}

		// If the exception wasn't due to a timeout, try to capture the
		// remaining stderr output for at most 2 seconds.
		if (errorKind != SpawnException::PRELOADER_STARTUP_TIMEOUT
		 && errorKind != SpawnException::APP_STARTUP_TIMEOUT
		 && stderrCapturer != NULL)
		{
			bool done = false;
			unsigned long long timeout = 2000;
			while (!done) {
				char buf[1024 * 32];
				unsigned int ret;

				try {
					ret = readExact(stderrCapturer->getFd(), buf,
						sizeof(buf), &timeout);
					if (ret == 0) {
						done = true;
					} else {
						stderrOutput.append(buf, ret);
					}
				} catch (const SystemException &e) {
					P_WARN("Stderr I/O capture error: " << e.what());
					done = true;
				} catch (const TimeoutException &) {
					done = true;
				}
			}
		}
		stderrCapturer.reset();

		// Now throw SpawnException with the captured stderr output
		// as error response.
		SpawnException e(msg,
			createErrorPageFromStderrOutput(msg, errorKind, stderrOutput),
			true,
			errorKind);
		e.setPreloaderCommand(getPreloaderCommandString());
		annotatePreloaderException(e, workDir);
		throwSpawnException(e, options);
	}

	void annotatePreloaderException(SpawnException &e, const WorkDirPtr &workDir) {
		if (workDir != NULL) {
			e.addAnnotations(workDir->readAll());
		}
	}

	bool preloaderStarted() const {
		return pid != -1;
	}

	void startPreloader() {
		TRACE_POINT();
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;
		assert(!preloaderStarted());
		P_DEBUG("Spawning new preloader: appRoot=" << options.appRoot);

		Config config(options);
		Json::Value extraArgs;
		Result result;
		HandshakeSession session(JOURNEY_TYPE_START_PRELOADER);
		session.context = context;
		session.config = &config;

		setConfigFromAppPoolOptions(config, extraArgs, options);
		HandshakePrepare(session, extraArgs).execute();

		vector<string> command = createRealPreloaderCommand(options, args);
		Pipe stdinChannel = createPipe(__FILE__, __LINE__);
		Pipe stdoutAndErrChannel = createPipe(__FILE__, __LINE__);
		adhoc_lve::LveEnter scopedLveEnter(LveLoggingDecorator::lveInitOnce(),
			session.uid,
			config.lveMinUid,
			LveLoggingDecorator::lveExitCallback);
		LveLoggingDecorator::logLveEnter(scopedLveEnter,
			session.uid,
			config.lveMinUid);

		pid_t pid = syscalls::fork();
		if (pid == 0) {
			purgeStdio(stdout);
			purgeStdio(stderr);
			resetSignalHandlersAndMask();
			disableMallocDebugging();
			int stdinCopy = dup2(stdinChannel.first, 3);
			int stdoutAndErrCopy = dup2(stdoutAndErrChannel.second, 4);
			dup2(stdinCopy, 0);
			dup2(stdoutAndErrCopy, 1);
			dup2(stdoutAndErrCopy, 2);
			closeAllFileDescriptors(2);
			execlp("./play/setupper",
				"./play/setupper",
				"spawn-env-setupper",
				session.workDir->getPath().c_str(),
				"--before",
				NULL);

			int e = errno;
			fprintf(stderr, "Cannot execute \"%s\": %s (errno=%d)\n",
				?????, strerror(e), e);
			fflush(stderr);
			_exit(1);

		} else if (pid == -1) {
			int e = errno;
			throw SystemException("Cannot fork a new process", e);

		} else {
			UPDATE_TRACE_POINT();
			scopedLveEnter.exit();

			P_LOG_FILE_DESCRIPTOR_PURPOSE(stdinChannel.second,
				"Preloader " << pid << " (" << options.appRoot << ") stdin");
			P_LOG_FILE_DESCRIPTOR_PURPOSE(stdoutAndErrChannel.first,
				"Preloader " << pid << " (" << options.appRoot << ") stdoutAndErr");

			UPDATE_TRACE_POINT();
			ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, pid));
			P_DEBUG("Preloader process forked for appRoot=" << options.appRoot
				<< ": PID " << pid);
			stdinChannel.first.close();
			stdoutAndErrChannel.second.close();

			HandshakePerform(session, pid, stdinChannel.second,
				stdoutAndErrChannel.first).execute();
			string socketAddress = findSocketAddress(session);
			{
				boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
				this->pid = pid;
				this->socketAddress = socketAddress;
				this->preloaderStdin = stdinChannel.second;
				this->preloaderAnnotations = preparation.workDir->readAll();
			}

			PipeWatcherPtr watcherr = boost::make_shared<PipeWatcher>(context,
				stdoutAndErrChannel.first, "output", pid);
			watcher->initialize();
			watcher->start();

			UPDATE_TRACE_POINT();
			guard.clear();
			P_INFO("Preloader for " << options.appRoot <<
				" started on PID " << pid <<
				", listening on " << socketAddress);
			return result;
		}
	}

	void stopPreloader() {
		TRACE_POINT();
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;

		if (!preloaderStarted()) {
			return;
		}

		safelyClose(preloaderStdin);
		if (timedWaitpid(pid, NULL, 5000) == 0) {
			P_DEBUG("Preloader did not exit in time, killing it...");
			syscalls::kill(pid, SIGKILL);
			syscalls::waitpid(pid, NULL, 0);
		}

		// Delete socket after the process has exited so that it
		// doesn't crash upon deleting a nonexistant file.
		if (getSocketAddressType(socketAddress) == SAT_UNIX) {
			string filename = parseUnixSocketAddress(socketAddress);
			syscalls::unlink(filename.c_str());
		}

		{
			boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
			pid = -1;
			socketAddress.clear();
			preloaderStdin = -1;
			preloaderAnnotations.clear();
		}
	}

	FileDescriptor connectToPreloader() {
		TRACE_POINT();
		FileDescriptor fd;

		try {
			fd.assign(connectToServer(socketAddress, __FILE__, __LINE__), NULL, 0);
		} catch (const SystemException &e) {
			session.journey.failedStep = PASSENGER_CORE_CONNECT_TO_PRELOADER;
			throw SpawnException(e, session.config, session.journey);
		} catch (const IOException &e) {
			throw SpawnException(e, session.config, session.journey);
		} catch (const TimeoutException &e) {
			throw SpawnException(e, session.config, session.journey);
		}

		P_LOG_FILE_DESCRIPTOR_PURPOSE(fd, "Preloader " << pid
			<< " (" << options.appRoot << ") connection");
		session.journey.doneSteps.insert(PASSENGER_CORE_CONNECT_TO_PRELOADER);
		return fd;
	}

	struct ForkResult {
		pid_t pid;
		FileDescriptor stdinFd;
		FileDescriptor stdoutAndErrFd;
		string alreadyReadStdoutAndErrData;

		ForkResult()
			: pid(-1)
			{ }

		ForkResult(pid_t _pid, const FileDescriptor &_stdinFd,
			const FileDescriptor &_stdoutAndErrFd,
			const string &_alreadyReadStdoutAndErrData)
			: pid(_pid),
			  stdinFd(_stdinFd),
			  stdoutAndErrFd(_stdoutAndErrFd),
			  alreadyReadStdoutAndErrData(_alreadyReadStdoutAndErrData)
			{ }
	};

	ForkResult invokeForkCommand(HandshakeSession &session) {
		TRACE_POINT();
		try {
			return invokeForkCommandFirstTry(session);
		} catch (const SpawnException &e) {
			if (e.getErrorKind() == SpawnException::TIMEOUT_ERROR) {
				throw;
			} else {
				P_WARN("An error occurred while spawning a process: " << e.what());
				P_WARN("The application preloader seems to have crashed,"
					" restarting it and trying again...");
				stopPreloader();
				startPreloader();
				ScopeGuard guard(boost::bind(&SmartSpawner::stopPreloader, this));
				ForkResult result = invokeForkCommandNormal(session);
				guard.clear();
				return result;
			}
		}
	}

	ForkResult invokeForkCommandFirstTry(HandshakeSession &session) {
		TRACE_POINT();
		FileDescriptor fd = connectToPreloader();
		sendForkCommand(session, fd);
		string line = readForkCommandResponse(session);
		Json::Value doc = parseForkCommandResponse(session, line);
		return handleForkCommandResponse(session, doc);
	}

	void sendForkCommand(HandshakeSession &session, const FileDescriptor &fd) {
		TRACE_POINT();
		Json::Value doc;

		doc["command"] = "spawn";
		doc["work_dir"] = session->workDir->getPath();

		try {
			writeExact(fd, Json::FastWriter.write(doc), &session.timeoutUsec);
		} catch (const SystemException &e) {
			session.journey.failedStep = PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER;
			throw SpawnException(e, session.config, session.journey);
		} catch (const TimeoutException &) {
			session.journey.failedStep = PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER;
			throw SpawnException(e, session.config, session.journey);
		}

		session.journey.doneSteps.insert(PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER);
	}

	string readForkCommandResponse(HandshakeSession &session, const FileDescriptor &fd) {
		TRACE_POINT();
		BufferedIO io(fd);
		string result;

		try {
			result = io.readLine(10240, &session.timeoutUsec);
		} catch (const SystemException &e) {
			session.journey.failedStep = PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER;
			throw SpawnException(e, session.config, session.journey);
		} catch (const TimeoutException &e) {
			session.journey.failedStep = PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER;
			throw SpawnException(e, session.config, session.journey);
		} catch (const SecurityException &) {
			session.journey.failedStep = PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER;
			SpawnException e(
				"The preloader process sent a response that exceeds the maximum size limit.",
				session.config,
				session.journey,
				SpawnException::INTERNAL_ERROR);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" this helper process sent a response that exceeded the"
				" internally-defined maximum size limit.</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"https://github.com/phusion/passenger/issues\">"
				"report this bug</a>."
				"</p>");
			throw e;
		}

		session.journey.doneSteps.insert(PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER);
		return result;
	}

	Json::Value parseForkCommandResponse(HandshakeSession &session, const string &data) {
		TRACE_POINT();
		Json::Value doc;
		Json::Reader reader;

		if (!reader.parse(data, &doc)) {
			UPDATE_TRACE_POINT();
			session.journey.failedStep = PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER;
			SpawnException e(
				"The preloader process sent an unparseable response: " + data,
				session.config,
				session.journey,
				SpawnException::INTERNAL_ERROR);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" this helper process sent a response that looks like"
				" gibberish.</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"https://github.com/phusion/passenger/issues\">"
				"report this bug</a>."
				"</p>");
			throw e;
		}

		if (!validateForkCommandResponse(doc)) {
			session.journey.failedStep = PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER;
			SpawnException e(
				"The preloader process sent a response that does not"
				" match the expected structure: " + stringifyJson(doc),
				session.config,
				session.journey,
				SpawnException::INTERNAL_ERROR);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" this helper process sent a response that does not match"
				" the structure that " SHORT_PROGRAM_NAME " expects.</p>"
				"<p>The response is as follows:</p>"
				"<pre>" + escapeHTML(doc.toStyledString()) + "</pre>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"https://github.com/phusion/passenger/issues\">"
				"report this bug</a>."
				"</p>");
			throw e;
		}

		session.journey.doneSteps.insert(PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER);
		return doc;
	}

	bool validateForkCommandResponse(const Json::Value &doc) const {
		if (!doc.isObject()) {
			return false;
		}
		if (!doc.isMember("result") || !doc["result"].isString()) {
			return false;
		}
		if (doc["result"].asString() == "ok") {
			if (!doc.isMember("pid") || !doc["pid"].isInt()) {
				return false;
			}
			return true;
		} else if (doc["result"].asString() == "error") {
			if (!doc.isMember("message") || !doc["message"].isString()) {
				return false;
			}
			return true;
		} else {
			return false;
		}
	}

	ForkResult handleForkCommandResponse(HandshakeSession &session, const Json::Value &doc) {
		TRACE_POINT();
		if (doc["result"].asString() == "ok") {
			return handleForkCommandResponseSuccess(session, doc);
		} else {
			P_ASSERT_EQ(doc["result"].asString(), "error");
			return handleForkCommandResponseError(session ,doc);
		}
	}

	ForkResult handleForkCommandResponseSuccess(HandshakeSession &session,
		const Json::Value &doc)
	{
		TRACE_POINT();
		pid_t spawnedPid = doc["pid"].asInt();
		ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, spawnedPid));

		FileDescriptor spawnedStdin, spawnedStdoutAndErr;
		BackgroundIOCapturerPtr stdoutAndErrCapturer;
		try {
			if (fileExists(session->responseDir + "/stdin")) {
				spawnedStdin = openFifoWithTimeout(
					session->responseDir + "/stdin",
					&session.timeoutUsec);
				P_LOG_FILE_DESCRIPTOR_PURPOSE(spawnedStdin,
					"App " << spawnedPid << " (" << options.appRoot
					<< ") stdin");
			}
			if (fileExists(session->responseDir + "/stdout_and_err")) {
				spawnedStdoutAndErr = openFifoWithTimeout(
					session->responseDir + "/stdout_and_err",
					&session.timeoutUsec);
				P_LOG_FILE_DESCRIPTOR_PURPOSE(spawnedStdoutAndErr,
					"App " << spawnedPid << " (" << options.appRoot
					<< ") stdoutAndErr");
				stdoutAndErrCapturer = boost::make_shared<BackgroundIOCapturer>(
					spawnedStdoutAndErr, pid);
				stdoutAndErrCapturer->start();
			}
		} catch (const std::exception &e) {
			session.journey.currentStep = PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER;
			session.journey.failedStep = PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER;
			session.journey.doneSteps.erase(PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER);
			session.journey.doneSteps.erase(PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER);
			throw SpawnException(e, session.config, session.journey);
		}

		// How do we know the preloader actually forked a process
		// instead of reporting the PID of a random other existing process?
		// For security reasons we perform a UID check.
		uid_t spawnedUid = getProcessUid(session, spawnedPid, stdoutAndErrCapturer);
		if (spawnedUid != session.uid) {
			session.journey.failedStep = PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER;
			SpawnException e(
				"The process that the preloader said it spawned, PID "
				+ toString(spawnedPid) + ", has UID " + toString(spawnedUid)
				+ ", but the expected UID is " + toString(session.uid),
				session.config,
				session.journey,
				SpawnException::INTERNAL_ERROR,
				string(),
				getBackgroundIOCapturerData(stdoutAndErrCapturer));
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application by communicating with a"
				" helper process that we call a \"preloader\". However,"
				" the web application process that the preloader started"
				" belongs to the wrong user. The UID of the web"
				" application process should be " + toString(session.uid)
				+ ", but is actually " + toString(session.uid) + ".</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is probably a bug in the preloader process. Please "
				"<a href=\"https://github.com/phusion/passenger/issues\">"
				"report this bug</a>."
				"</p>");
			throw e;
		}

		stdoutAndErrCapturer->stop();
		session.journey.doneSteps.insert(PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER);
		guard.clear();
		return ForkResult(spawnedPid, spawnedStdin, spawnedStdoutAndErr,
			getBackgroundIOCapturerData(stdoutAndErrCapturer));
	}

	ForkResult handleForkCommandResponseError(HandshakeSession &session,
		const Json::Value &doc)
	{
		session.journey.failedStep = PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER;
		SpawnException e(
			"An error occured while starting the web application: "
			+ doc["message"].asString(),
			session.config,
			session.journey,
			SpawnException::INTERNAL_ERROR,
			doc["message"].asString());
		e.setProblemDescriptionHTML(
			"<p>The " PROGRAM_NAME " application server tried to"
			" start the web application by communicating with a"
			" helper process that we call a \"preloader\". However, "
			" this helper process reported an error:</p>"
			"<pre>" + escapeHTML(doc["message"].asString()) + "</pre>");
		e.setSolutionDescriptionHTML(
			"<p class=\"sole-solution\">"
			"Please try troubleshooting the problem by studying the"
			" <strong>error message</strong> and the"
			" <strong>diagnostics</strong> reports. You can also"
			" consult <a href=\"" SUPPORT_URL "\">the " SHORT_PROGRAM_NAME
			" support resources</a> for help.</p>");
		throw e;
	}

	string getBackgroundIOCapturerData(const BackgroundIOCapturerPtr &capturer) const {
		if (capturer != NULL) {
			// Sleep shortly to allow the child process to finish writing logs.
			syscalls::usleep(50000);
			return capturer->getData();
		} else {
			return string();
		}
	}

	uid_t getProcessUid(HandshakeSession &session, pid_t pid,
		const BackgroundIOCapturerPtr &stdoutAndErrCapturer)
	{
		uid_t uid;

		try {
			vector<pid_t> pids;
			pids.push_back(pid);
			ProcessMetricsMap result = ProcessMetricsCollector().collect(pids);
			uid = result[pid].uid;
		} catch (const ParseException &) {
			session.journey.failedStep = PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER;
			SpawnException e(
				"Unable to query the UID of spawned application process "
					+ toString(pid) + ": error parsing 'ps' output",
				session.config,
				session.journey,
				SpawnException::INTERNAL_ERROR);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application. As part of the starting"
				" sequence, " SHORT_PROGRAM_NAME " also tried to query"
				" the system user ID of the web application process"
				" using the operating system's \"ps\" tool. However,"
				" this tool returned output that " SHORT_PROGRAM_NAME
				" could not understand.</p>");
			e.setSolutionDescriptionHTML(
				createSolutionDescriptionForProcessMetricsCollectionError());
			throw e;
		} catch (const SystemException &e) {
			session.journey.failedStep = PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER;
			SpawnException e(
				"Unable to query the UID of spawned application process "
					+ toString(pid) + "; error capturing 'ps' output: "
					+ e.what(),
				session.config,
				session.journey,
				SpawnException::OPERATING_SYSTEM_ERROR);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application. As part of the starting"
				" sequence, " SHORT_PROGRAM_NAME " also tried to query"
				" the system user ID of the web application process."
				" This is done by using the operating system's \"ps\""
				" tool and by querying operating system APIs and special"
				" files. However, an error was encountered while doing"
				" one of those things.</p>"
				"<p>The error returned by the operating system is as follows:</p>"
				"<pre>" + escapeHTML(e.what()) + "</pre>");
			e.setSolutionDescriptionHTML(
				createSolutionDescriptionForProcessMetricsCollectionError());
			throw e;
		}

		if (uid == -1) {
			if (osProcessExists(pid)) {
				session.journey.failedStep = PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER;
				SpawnException e(
					"Unable to query the UID of spawned application process "
						+ toString(pid) + ": 'ps' did not report information"
						" about this process",
					session.config,
					session.journey,
					SpawnException::INTERNAL_ERROR);
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application. As part of the starting"
					" sequence, " SHORT_PROGRAM_NAME " also tried to query"
					" the system user ID of the web application process"
					" using the operating system's \"ps\" tool. However,"
					" this tool did not return any information about"
					" the web application process.</p>");
				e.setSolutionDescriptionHTML(
					createSolutionDescriptionForProcessMetricsCollectionError());
				throw e;
			} else {
				session.journey.failedStep = PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER;
				SpawnException e(
					"The application process spawned from the preloader"
					" seems to have exited prematurely",
					session.config,
					session.journey,
					SpawnException::INTERNAL_ERROR,
					string(),
					getBackgroundIOCapturerData(stdoutAndErrCapturer));
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application. As part of the starting"
					" sequence, " SHORT_PROGRAM_NAME " also tried to query"
					" the system user ID of the web application process"
					" using the operating system's \"ps\" tool. However,"
					" this tool did not return any information about"
					" the web application process.</p>");
				e.setSolutionDescriptionHTML(
					createSolutionDescriptionForProcessMetricsCollectionError());
				throw e;
			}
		} else {
			return uid;
		}
	}

	static string createSolutionDescriptionForProcessMetricsCollectionError() {
		const char *path = getenv("PATH");
		if (path == NULL || path[0] == '\0') {
			path = "(empty)";
		}
		return "<div class=\"multiple-solutions\">"

			"<h3>Check whether the \"ps\" tool is installed and accessible by "
			SHORT_PROGRAM_NAME "</h3>"
			"<p>Maybe \"ps\" is not installed. Or maybe it is installed, but "
			SHORT_PROGRAM_NAME " cannot find it inside its PATH. Or"
			" maybe filesystem permissions disallow " SHORT_PROGRAM_NAME
			" from accessing \"ps\". Please check all these factors and"
			" fix them if necessary.</p>"
			"<p>" SHORT_PROGRAM_NAME "'s PATH is:</p>"
			"<pre>" + escapeHTML(path) + "</pre>"

			"<h3>Check whether the server is low on resources</h3>"
			"<p>Maybe the server is currently low on resources. This would"
			" cause the \"ps\" tool to encounter errors. Please study the"
			" <em>error message</em> and the <em>diagnostics reports</em> to"
			" verify whether this is the case. Key things to check for:</p>"
			"<ul>"
			"<li>Excessive CPU usage</li>"
			"<li>Memory and swap</li>"
			"<li>Ulimits</li>"
			"</ul>"
			"<p>If the server is indeed low on resources, find a way to"
			" free up some resources.</p>"

			"<h3>Check whether /proc is mounted</h3>"
			"<p>On many operating systems including Linux and FreeBSD, \"ps\""
			" only works if /proc is mounted. Please check this.</p>"

			"<h3>Still no luck?</h3>"
			"<p>Please try troubleshooting the problem by studying the"
			" <em>diagnostics</em> reports.</p>"

			"</div>";
	}

	static FileDescriptor openFifoWithTimeout(const string &path,
		unsigned long long &timeout)
	{
		TRACE_POINT();
		FileDescriptor fd;
		int errcode;
		oxt::thread thr(
			boost::bind(openFifoWithTimeoutThreadMain, path, &fd, &errcode),
			"FIFO opener: " + path, 1024 * 128);

		MonotonicTimeUsec startTime = SystemTime::getMonotonicUsec();
		ScopeGuard guard(boost::bind(adjustTimeout, startTime, &timeout));

		try {
			UPDATE_TRACE_POINT();
			if (thr.try_join_until(boost::posix_time::microseconds(timeout))) {
				if (fd == -1) {
					throw SystemException("Cannot open FIFO " + path, errcode);
				} else {
					return fd;
				}
			} else {
				boost::this_thread::disable_interruption di;
				boost::this_thread::disable_syscall_interruption dsi;
				thr.interrupt_and_join();
				throw TimeoutException("Timeout opening FIFO " + path);
			}
		} catch (const boost::thread_interrupted &) {
			boost::this_thread::disable_interruption di;
			boost::this_thread::disable_syscall_interruption dsi;
			UPDATE_TRACE_POINT();
			thr.interrupt_and_join();
			throw;
		} catch (const boost::system_error &e) {
			throw SystemException(e.what(), e.code().value());
		}
	}

	static void openFifoWithTimeoutThreadMain(const string path,
		FileDescriptor *fd, int *errcode)
	{
		TRACE_POINT();
		*fd = syscalls::open(path.c_str(), O_RDONLY);
		*errcode = errno;
	}

	static void adjustTimeout(MonotonicTimeUsec startTime, unsigned long long *timeout) {
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;
		MonotonicTimeUsec now = SystemTime::getMonotonicUsec();
		assert(now >= startTime);
		MonotonicTimeUsec diff = startTime - now;
		if (*timeout >= diff) {
			*timeout -= diff;
		} else {
			*timeout = 0;
		}
	}

protected:
	virtual void annotateAppSpawnException(SpawnException &e, NegotiationDetails &details) {
		Spawner::annotateAppSpawnException(e, details);
		e.addAnnotations(preloaderAnnotations);
	}

public:
	SmartSpawner(Context *context,
		const vector<string> &_preloaderCommand,
		const AppPoolOptions &_options)
		: Spawner(context),
		  preloaderCommand(_preloaderCommand)
	{
		if (preloaderCommand.size() < 2) {
			throw ArgumentException("preloaderCommand must have at least 2 elements");
		}

		options    = _options.copyAndPersist().detachFromUnionStationTransaction();
		pid        = -1;
		m_lastUsed = SystemTime::getUsec();
	}

	virtual ~SmartSpawner() {
		boost::lock_guard<boost::mutex> l(syncher);
		stopPreloader();
	}

	virtual Result spawn(const AppPoolOptions &options) {
		TRACE_POINT();
		P_ASSERT_EQ(options.appType, this->options.appType);
		P_ASSERT_EQ(options.appRoot, this->options.appRoot);

		P_DEBUG("Spawning new process: appRoot=" << options.appRoot);
		possiblyRaiseInternalError(options);

		{
			boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
			m_lastUsed = SystemTime::getUsec();
		}
		UPDATE_TRACE_POINT();
		boost::lock_guard<boost::mutex> l(syncher);
		if (!preloaderStarted()) {
			UPDATE_TRACE_POINT();
			startPreloader();
		}

		UPDATE_TRACE_POINT();
		Config config(options);
		Json::Value extraArgs;
		Result result;
		HandshakeSession session;
		session.context = context;
		session.config = &config;

		setConfigFromAppPoolOptions(config, extraArgs, options);
		HandshakePrepare(session, extraArgs).execute();

		ForkResult forkResult = invokeForkCommand(session);
		ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, forkResult.pid));
		P_DEBUG("Process forked for appRoot=" << options.appRoot << ": PID " << forkResult.pid);
		HandshakePerform(session, forkResult.pid, forkResult.stdinFd,
			forkResult.stdoutAndErrFd, forkResult.stdoutAndErrCapturer).execute();
		guard.clear();
		P_DEBUG("Process spawning done: appRoot=" << options.appRoot <<
			", pid=" << forkResult.pid);
		return result;
	}

	virtual bool cleanable() const {
		return true;
	}

	virtual void cleanup() {
		TRACE_POINT();
		{
			boost::lock_guard<boost::mutex> l(simpleFieldSyncher);
			m_lastUsed = SystemTime::getUsec();
		}
		boost::lock_guard<boost::mutex> lock(syncher);
		stopPreloader();
	}

	virtual unsigned long long lastUsed() const {
		boost::lock_guard<boost::mutex> lock(simpleFieldSyncher);
		return m_lastUsed;
	}

	pid_t getPreloaderPid() const {
		boost::lock_guard<boost::mutex> lock(simpleFieldSyncher);
		return pid;
	}
};


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_SMART_SPAWNER_H_ */
