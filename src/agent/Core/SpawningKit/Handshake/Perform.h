/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2016-2017 Phusion Holding B.V.
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
#ifndef _PASSENGER_SPAWNING_KIT_HANDSHAKE_PERFORM_H_
#define _PASSENGER_SPAWNING_KIT_HANDSHAKE_PERFORM_H_

#include <boost/thread.hpp>
#include <boost/make_shared.hpp>
#include <boost/bind.hpp>
#include <oxt/thread.hpp>
#include <oxt/system_calls.hpp>
#include <oxt/backtrace.hpp>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstddef>
#include <cerrno>

#include <sys/types.h>

#include <Constants.h>
#include <Exceptions.h>
#include <FileDescriptor.h>
#include <Utils/ScopeGuard.h>
#include <Utils/SystemTime.h>
#include <Core/SpawningKit/Config.h>
#include <Core/SpawningKit/Exceptions.h>
#include <Core/SpawningKit/Handshake/BackgroundIOCapturer.h>
#include <Core/SpawningKit/Handshake/Session.h>

namespace Passenger {
namespace SpawningKit {

using namespace std;
using namespace oxt;


class HandshakePerform {
private:
	enum FinishState {
		// The app hasn't finished spawning yet.
		NOT_FINISHED,
		// The app has successfully finished spawning.
		FINISH_SUCCESS,
		// The app has finished spawning with an error.
		FINISH_ERROR,
		// An internal error occurred in watchFinishSignal().
		FINISH_INTERNAL_ERROR
	};

	HandshakeSession &session;
	Config * const config;
	const pid_t pid;
	const FileDescriptor stdinFd;
	const FileDescriptor stdoutAndErrFd;
	const string alreadyReadStdoutAndErrData;


	/**
	 * These objects captures the process's stdout and stderr while handshake is
	 * in progress. If handshaking fails, then any output captured by these objects
	 * will be stored into the resulting SpawnException's error page.
	 */
	BackgroundIOCapturerPtr stdoutAndErrCapturer;

	boost::mutex syncher;
	boost::condition_variable cond;

	oxt::thread *processExitWatcher;
	oxt::thread *finishSignalWatcher;
	bool processExited;
	FinishState finishState;
	string finishSignalWatcherErrorMessage;
	SpawnException::ErrorKind finishSignalWatcherErrorKind;

	oxt::thread *socketPingabilityWatcher;
	bool socketIsNowPingable;


	void initializeStdchannelsCapturing() {
		if (stdoutAndErrFd != -1) {
			stdoutAndErrCapturer = boost::make_shared<BackgroundIOCapturer>(
				stdoutAndErrFd, pid, "output", alreadyReadStdoutAndErrData);
			stdoutAndErrCapturer->setEndReachedCallback(boost::bind(
				&HandshakePerform::wakeupEventLoop, this));
			stdoutAndErrCapturer->start();
		}
	}

	void startWatchingProcessExit() {
		processExitWatcher = new oxt::thread(
			boost::bind(&HandshakePerform::watchProcessExit, this),
			"SpawningKit: process exit watcher", 64 * 1024);
	}

	void watchProcessExit() {
		TRACE_POINT();
		int ret = syscalls::waitpid(pid, NULL, 0);
		if (ret >= 0 || errno == EPERM) {
			boost::lock_guard<boost::mutex> l(syncher);
			processExited = true;
			wakeupEventLoop();
		}
	}

	void startWatchingFinishSignal() {
		finishSignalWatcher = new oxt::thread(
			boost::bind(&HandshakePerform::watchFinishSignal, this),
			"SpawningKit: finish signal watcher", 64 * 1024);
	}

	void watchFinishSignal() {
		TRACE_POINT();
		try {
			string path = session.workDir->getPath() + "/finish";
			int fd = syscalls::open(path.c_str(), O_RDONLY);
			if (fd == -1) {
				int e = errno;
				throw FileSystemException("Error opening FIFO " + path,
					e, path);
			}
			FdGuard guard(fd, __FILE__, __LINE__);

			char buf = '0';
			ssize_t ret = syscalls::read(fd, &buf, 1);
			if (ret == -1) {
				int e = errno;
				throw FileSystemException("Error reading from FIFO " + path,
					e, path);
			}

			guard.runNow();

			boost::lock_guard<boost::mutex> l(syncher);
			if (buf == '1') {
				finishState = FINISH_SUCCESS;
			} else {
				finishState = FINISH_ERROR;
			}
			wakeupEventLoop();
		} catch (const std::exception &e) {
			boost::lock_guard<boost::mutex> l(syncher);
			finishState = FINISH_INTERNAL_ERROR;
			finishSignalWatcherErrorMessage = e.what();
			if (dynamic_cast<const SystemException *>(&e) != NULL
			 || dynamic_cast<const IOException *>(&e) != NULL)
			{
				finishSignalWatcherErrorKind =
					SpawnException::OPERATING_SYSTEM_ERROR;
			} else {
				finishSignalWatcherErrorKind =
					SpawnException::INTERNAL_ERROR;
			}
			wakeupEventLoop();
		}
	}

	void startWatchingSocketPingability() {
		socketPingabilityWatcher = new oxt::thread(
			boost::bind(&HandshakePerform::watchSocketPingability, this),
			"SpawningKit: socket pingability watcher", 64 * 1024);
	}

	void watchSocketPingability() {
		TRACE_POINT();

		while (true) {
			unsigned long long timeout = 100000;

			if (pingTcpServer("127.0.0.1", session.expectedStartPort, &timeout)) {
				boost::lock_guard<boost::mutex> l(syncher);
				socketIsNowPingable = true;
				finishState = FINISH_SUCCESS;
				wakeupEventLoop();
			} else {
				syscalls::usleep(50000);
			}
		}
	}

	void waitUntilSpawningFinished(boost::unique_lock<boost::mutex> &l) {
		TRACE_POINT();
		bool done;

		do {
			boost::this_thread::interruption_point();
			done = checkCurrentState();
			if (!done) {
				MonotonicTimeUsec begin = SystemTime::getMonotonicUsec();
				cond.timed_wait(l, posix_time::microseconds(session.timeoutUsec));
				MonotonicTimeUsec end = SystemTime::getMonotonicUsec();
				if (end - begin > session.timeoutUsec) {
					session.timeoutUsec = 0;
				} else {
					session.timeoutUsec -= end - begin;
				}
			}
		} while (!done);
	}

	bool checkCurrentState() {
		if ((stdoutAndErrCapturer != NULL && stdoutAndErrCapturer->isStopped())
		 || processExited)
		{
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.loadInfoFromResponseDir(session.responseDir);
			throw SpawnException(
				"An error occurred while spawning an application process.",
				config,
				session.journey,
				SpawnException::DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR,
				string(),
				getStdouterrData(),
				session.responseDir,
				true);
		}

		if (session.timeoutUsec == 0) {
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.loadInfoFromResponseDir(session.responseDir);
			throw SpawnException(
				"A timeout occurred while spawning an application process.",
				config,
				session.journey,
				SpawnException::TIMEOUT_ERROR,
				string(),
				getStdouterrData(),
				session.responseDir,
				true);
		}

		return (config->genericApp && socketIsNowPingable)
			|| (!config->genericApp && finishState != NOT_FINISHED);
	}

	Result handleResponse() {
		TRACE_POINT();
		switch (finishState) {
		case FINISH_SUCCESS:
			return handleSuccessResponse();
		case FINISH_ERROR:
			handleErrorResponse();
			return Result(); // Never reached, shut up compiler warning.
		case FINISH_INTERNAL_ERROR:
			handleInternalError();
			return Result(); // Never reached, shut up compiler warning.
		default:
			P_BUG("Unknown finishState " + toString((int) finishState));
			return Result(); // Never reached, shut up compiler warning.
		}
	}

	Result handleSuccessResponse() {
		TRACE_POINT();
		Result &result = session.result;
		vector<StaticString> internalFieldErrors, appSuppliedFieldErrors;

		result.pid = pid;
		result.stdoutAndErrFd = stdoutAndErrFd;
		result.spawnEndTime = SystemTime::getUsec();
		result.spawnEndTimeMonotonic = SystemTime::getMonotonicUsec();
		try {
			result.loadPropertiesFromResponseDir(session.responseDir);
		} catch (const VariantMap::MissingKeyException &e) {
			appSuppliedFieldErrors.push_back(e.what());
			throwSpawnExceptionBecauseOfResultValidationErrors(internalFieldErrors,
				appSuppliedFieldErrors);
		} catch (const ConfigurationException &e) {
			appSuppliedFieldErrors.push_back(e.what());
			throwSpawnExceptionBecauseOfResultValidationErrors(internalFieldErrors,
				appSuppliedFieldErrors);
		}

		UPDATE_TRACE_POINT();
		if (socketIsNowPingable) {
			assert(config->genericApp || config->findFreePort);
			Result::Socket socket;
			socket.name = "main";
			socket.address = "tcp://127.0.0.1:" + toString(session.expectedStartPort);
			socket.protocol = "http_session";
			socket.concurrency = -1; // unknown concurrency
			result.sockets.push_back(socket);
		} else if (result.sockets.empty()) {
			throwSpawnExceptionBecauseAppDidNotProvideSockets();
		}

		if (result.validate(internalFieldErrors, appSuppliedFieldErrors)) {
			return result;
		} else {
			throwSpawnExceptionBecauseOfResultValidationErrors(internalFieldErrors,
				appSuppliedFieldErrors);
			abort(); // never reached, shut up compiler warning
		}
	}

	void handleErrorResponse() {
		TRACE_POINT();
		sleepShortlyToCaptureMoreStdoutStderr();
		session.journey.loadInfoFromResponseDir(session.responseDir);
		throw SpawnException(
			"The web application aborted with an error during startup.",
			config,
			session.journey,
			SpawnException::DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR,
			string(),
			getStdouterrData(),
			session.responseDir,
			true);
	}

	void handleInternalError() {
		TRACE_POINT();
		sleepShortlyToCaptureMoreStdoutStderr();
		session.journey.failedStep = PASSENGER_CORE_DURING_HANDSHAKE;
		throw SpawnException(
			"An internal error occurred while spawning an application process: "
				+ finishSignalWatcherErrorMessage,
			config,
			journey,
			finishSignalWatcherErrorKind,
			finishSignalWatcherErrorMessage,
			getStdouterrData(),
			session.responseDir);
	}

	void wakeupEventLoop() {
		cond.notify_all();
	}

	string getStdouterrData() const {
		if (stdoutAndErrCapturer != NULL) {
			return stdoutAndErrCapturer->getData();
		} else {
			return "(not available)";
		}
	}

	void sleepShortlyToCaptureMoreStdoutStderr() const {
		syscalls::usleep(50000);
	}

	void throwSpawnExceptionBecauseAppDidNotProvideSockets() {
		assert(!config->genericApp);
		sleepShortlyToCaptureMoreStdoutStderr();
		if (config->startsUsingWrapper) {
			session.journey.failedStep = SUBPROCESS_WRAPPER_PREPARATION;
			SpawnException e(
				"Error spawning the web application: the application wrapper"
				" did not report any sockets to receive requests on.",
				config,
				session.journey,
				SpawnException::INTERNAL_ERROR,
				string(),
				session.responseDir);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application through a " SHORT_PROGRAM_NAME
				"-internal helper tool (in technical terms: the wrapper), "
				" but " SHORT_PROGRAM_NAME " encountered a bug"
				" in this helper tool. " SHORT_PROGRAM_NAME " expected"
				" the helper tool to report a socket to receive requests"
				" on, but the helper tool finished its startup sequence"
				" without reporting a socket.</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is a bug in " SHORT_PROGRAM_NAME "."
				" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
				" to the " SHORT_PROGRAM_NAME " authors.</p>");
			throw e;
		} else {
			session.journey.failedStep = SUBPROCESS_APP_LOAD_OR_EXEC;
			SpawnException e(
				"Error spawning the web application: the application"
				" did not report any sockets to receive requests on.",
				config,
				session.journey,
				SpawnException::INTERNAL_ERROR,
				string(),
				session.responseDir);
			e.setProblemDescriptionHTML(
				"<p>The " PROGRAM_NAME " application server tried"
				" to start the web application, but encountered a bug"
				" in the application. " SHORT_PROGRAM_NAME " expected"
				" the application to report a socket to receive requests"
				" on, but the application finished its startup sequence"
				" without reporting a socket.</p>");
			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"Since this is a bug in the web application, please "
				"report this problem to the application's developer. "
				"This problem is outside " SHORT_PROGRAM_NAME "'s "
				"control.</p>");
			throw e;
		}
	}

	void throwSpawnExceptionBecauseOfResultValidationErrors(
		const vector<StaticString> &internalFieldErrors,
		const vector<StaticString> &appSuppliedFieldErrors)
	{
		string message;
		vector<StaticString>::const_iterator it, end;

		sleepShortlyToCaptureMoreStdoutStderr();

		if (!internalFieldErrors.empty()) {
			assert(!config->startsUsingWrapper);

			end = internalFieldErrors.end();

			session.journey.failedStep = PASSENGER_CORE_DURING_HANDSHAKE;
			SpawnException e("Error spawning the web application:"
				" a bug in " SHORT_PROGRAM_NAME " caused the"
				" spawn result to be invalid: "
				+ toString(internalFieldErrors),
				config,
				session.journey,
				SpawnException::INTERNAL_ERROR,
				string(),
				session.responseDir);

			message = "<p>The " PROGRAM_NAME " application server tried"
				" to start the web application, but encountered a bug"
				" in " SHORT_PROGRAM_NAME " itself. The errors are as"
				" follows:</p>"
				"<ul>";
			for (it = internalFieldErrors.begin(); it != end; it++) {
				message.append("<li>" + escapeHTML(*it) + "</li>");
			}
			message.append("</ul>");
			e.setProblemDescriptionHTML(message);

			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is a bug in " SHORT_PROGRAM_NAME "."
				" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
				" to the " SHORT_PROGRAM_NAME " authors.</p>");

			throw e;

		} else if (config->startsUsingWrapper) {
			end = appSuppliedFieldErrors.end();

			session.journey.failedStep = SUBPROCESS_WRAPPER_PREPARATION;
			SpawnException e("Error spawning the web application:"
				" a bug in " SHORT_PROGRAM_NAME " caused the"
				" spawn result to be invalid: "
				+ toString(appSuppliedFieldErrors),
				config,
				session.journey,
				SpawnException::INTERNAL_ERROR,
				string(),
				session.responseDir);

			message = "<p>The " PROGRAM_NAME " application server tried"
				" to start the web application through a " SHORT_PROGRAM_NAME
				"-internal helper tool (in technical terms: the wrapper), "
				" but " SHORT_PROGRAM_NAME " encountered a bug"
				" in this helper tool. " SHORT_PROGRAM_NAME " expected"
				" the helper tool to communicate back various information"
				" about the application's startup sequence, but the tool"
				" did not communicate back correctly."
				" The errors are as follows:</p>"
				"<ul>";
			for (it = appSuppliedFieldErrors.begin(); it != end; it++) {
				message.append("<li>" + escapeHTML(*it) + "</li>");
			}
			message.append("</ul>");
			e.setProblemDescriptionHTML(message);

			e.setSolutionDescriptionHTML(
				"<p class=\"sole-solution\">"
				"This is a bug in " SHORT_PROGRAM_NAME "."
				" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
				" to the " SHORT_PROGRAM_NAME " authors.</p>");

			throw e;

		} else {
			end = appSuppliedFieldErrors.end();

			session.journey.failedStep = SUBPROCESS_APP_LOAD_OR_EXEC;
			SpawnException e("Error spawning the web application:"
				" the application's spawn response is invalid: "
				+ toString(appSuppliedFieldErrors),
				config,
				session.journey,
				SpawnException::INTERNAL_ERROR,
				string(),
				session.responseDir);

			message = "<p>The " PROGRAM_NAME " application server tried"
				" to start the web application, but encountered a bug"
				" in the application. " SHORT_PROGRAM_NAME " expected"
				" the application to communicate back various information"
				" about its startup sequence, but the application"
				" did not communicate back that correctly."
				" The errors are as follows:</p>"
				"<ul>";
			for (it = appSuppliedFieldErrors.begin(); it != end; it++) {
				message.append("<li>" + escapeHTML(*it) + "</li>");
			}
			message.append("</ul>");
			e.setProblemDescriptionHTML(message);

			if (config->genericApp) {
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"Since this is a bug in the web application, please "
					"report this problem to the application's developer. "
					"This problem is outside " SHORT_PROGRAM_NAME "'s "
					"control.</p>");
			} else {
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"This is a bug in " SHORT_PROGRAM_NAME "."
					" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
					" to the " SHORT_PROGRAM_NAME " authors.</p>");
			}

			throw e;
		}
	}

	void cleanup() {
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;
		TRACE_POINT();

		if (processExitWatcher != NULL) {
			processExitWatcher->interrupt_and_join();
			delete processExitWatcher;
			processExitWatcher = NULL;
		}
		if (finishSignalWatcher != NULL) {
			finishSignalWatcher->interrupt_and_join();
			delete finishSignalWatcher;
			finishSignalWatcher = NULL;
		}
		if (socketPingabilityWatcher != NULL) {
			socketPingabilityWatcher->interrupt_and_join();
			delete socketPingabilityWatcher;
			socketPingabilityWatcher = NULL;
		}
		if (stdoutAndErrCapturer != NULL) {
			stdoutAndErrCapturer->stop();
		}
	}

public:
	HandshakePerform(HandshakeSession &_session, pid_t _pid,
		const FileDescriptor &_stdinFd = FileDescriptor(),
		const FileDescriptor &_stdoutAndErrFd = FileDescriptor(),
		const string &_alreadyReadStdoutAndErrData = string())
		: session(_session),
		  config(session.config),
		  pid(_pid),
		  stdinFd(_stdinFd),
		  stdoutAndErrFd(_stdoutAndErrFd),
		  alreadyReadStdoutAndErrData(_alreadyReadStdoutAndErrData),
		  processExitWatcher(NULL),
		  finishSignalWatcher(NULL),
		  processExited(false),
		  finishState(NOT_FINISHED),
		  socketPingabilityWatcher(NULL),
		  socketIsNowPingable(false)
		{ }

	Result execute() {
		TRACE_POINT();
		ScopeGuard guard(boost::bind(&HandshakePerform::cleanup, this));

		session.journey.currentStep = PASSENGER_CORE_HANDSHAKE_PERFORM;

		try {
			initializeStdchannelsCapturing();
			startWatchingProcessExit();
			if (config->genericApp || config->findFreePort) {
				startWatchingSocketPingability();
			}
			if (!config->genericApp) {
				startWatchingFinishSignal();
			}
		} catch (const std::exception &e) {
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.failedStep = PASSENGER_CORE_HANDSHAKE_PERFORM;
			throw SpawnException(e, config, session.journey,
				getStdouterrData(), session.responseDir);
		}

		UPDATE_TRACE_POINT();
		try {
			boost::unique_lock<boost::mutex> l(syncher);
			waitUntilSpawningFinished(l);
			return handleResponse();
		} catch (const SpawnException &) {
			throw;
		} catch (const std::exception &e) {
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.failedStep = PASSENGER_CORE_DURING_HANDSHAKE;
			throw SpawnException(e, config, session.journey,
				getStdouterrData(), session.responseDir);
		}

		session.journey.doneSteps.insert(PASSENGER_CORE_HANDSHAKE_PERFORM);
	}
};


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_HANDSHAKE_PERFORM_H_ */
