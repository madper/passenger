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
#include <dirent.h>

#include <Constants.h>
#include <Exceptions.h>
#include <FileDescriptor.h>
#include <Utils.h>
#include <Utils/ScopeGuard.h>
#include <Utils/SystemTime.h>
#include <Utils/StrIntUtils.h>
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
	ErrorCategory finishSignalWatcherErrorCategory;

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
			finishSignalWatcherErrorCategory =
				inferErrorCategoryFromAnotherException(e,
					SPAWNING_KIT_HANDSHAKE_PERFORM);
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
			loadJourneyStateFromResponseDir();
			SpawnException e(
				inferErrorCategoryFromResponseDir(INTERNAL_ERROR),
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			loadSubprocessErrorMessagesAndAnnotations(e);
			throw e.finalize();
		}

		if (session.timeoutUsec == 0) {
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.setStepErrored(SPAWNING_KIT_HANDSHAKE_PERFORM);
			loadJourneyStateFromResponseDir();
			SpawnException e(
				TIMEOUT_ERROR,
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			loadSubprocessErrorMessagesAndAnnotations(e);
			throw e.finalize();
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
		loadJourneyStateFromResponseDir();
		SpawnException e(
			inferErrorCategoryFromResponseDir(INTERNAL_ERROR),
			session.journey,
			config);
		e.setSummary("The web application aborted with an error during startup.");
		e.setStdoutAndErrData(getStdoutErrData());
		loadSubprocessErrorMessagesAndAnnotations(e);
		throw e.finalize();
	}

	void handleInternalError() {
		TRACE_POINT();
		sleepShortlyToCaptureMoreStdoutStderr();
		session.journey.setStepErrored(SPAWNING_KIT_HANDSHAKE_PERFORM);
		loadJourneyStateFromResponseDir();
		SpawnException e(
			finishSignalWatcherErrorCategory,
			session.journey,
			config);
		e.setSummary("An internal error occurred while spawning an application process: "
			+ finishSignalWatcherErrorMessage);
		e.setAdvancedProblemDetails(finishSignalWatcherErrorMessage);
		e.setStdoutAndErrData(getStdoutErrData());
		throw e.finalize();
	}

	void wakeupEventLoop() {
		cond.notify_all();
	}

	string getStdoutErrData() const {
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

		if (!config->genericApp && config->startsUsingWrapper) {
			session.journey.setStepErrored(SUBPROCESS_WRAPPER_PREPARATION);
			loadJourneyStateFromResponseDir();

			SpawnException e(
				INTERNAL_ERROR,
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			loadAnnotationsFromEnvDumpDir(e);

			if (config->wrapperSuppliedByThirdParty) {
				e.setSummary("Error spawning the web application:"
					" a third-party application wrapper did not"
					" report any sockets to receive requests on.");
			} else {
				e.setSummary("Error spawning the web application:"
					" a " SHORT_PROGRAM_NAME "-internal application"
					" wrapper did not report any sockets to receive"
					" requests on.");
			}

			if (config->wrapperSuppliedByThirdParty) {
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through a helper tool "
					" called the \"wrapper\". This helper tool is not part of "
					SHORT_PROGRAM_NAME ". " SHORT_PROGRAM_NAME " expected"
					" the helper tool to report a socket to receive requests"
					" on, but the helper tool finished its startup sequence"
					" without reporting a socket.</p>");
			} else {
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through a " SHORT_PROGRAM_NAME
					"-internal helper tool called the \"wrapper\", "
					" but " SHORT_PROGRAM_NAME " encountered a bug"
					" in this helper tool. " SHORT_PROGRAM_NAME " expected"
					" the helper tool to report a socket to receive requests"
					" on, but the helper tool finished its startup sequence"
					" without reporting a socket.</p>");
			}

			if (config->wrapperSuppliedByThirdParty) {
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"This is a bug in the wrapper, so please contact the author of"
					" the wrapper. This problem is outside " SHORT_PROGRAM_NAME
					"'s control. Below follows the command that "
					SHORT_PROGRAM_NAME " tried to execute, so that you can infer"
					" which wrapper was used:</p>"
					"<pre>" + escapeHTML(config->startCommand) + "</pre>");
			} else {
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"This is a bug in " SHORT_PROGRAM_NAME "."
					" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
					" to the " SHORT_PROGRAM_NAME " authors.</p>");
			}

			throw e.finalize();

		} else {
			session.journey.setStepErrored(SUBPROCESS_APP_LOAD_OR_EXEC);
			loadJourneyStateFromResponseDir();

			SpawnException e(
				INTERNAL_ERROR,
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			loadAnnotationsFromEnvDumpDir(e);

			e.setSummary("Error spawning the web application: the application"
				" did not report any sockets to receive requests on.");
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

			throw e.finalize();
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
			session.journey.setStepErrored(SPAWNING_KIT_HANDSHAKE_PERFORM);
			loadJourneyStateFromResponseDir();

			SpawnException e(
				INTERNAL_ERROR,
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			e.setAdvancedProblemDetails(toString(internalFieldErrors));

			e.setSummary("Error spawning the web application:"
				" a bug in " SHORT_PROGRAM_NAME " caused the"
				" spawn result to be invalid: "
				+ toString(internalFieldErrors));

			message = "<p>The " PROGRAM_NAME " application server tried"
				" to start the web application, but encountered a bug"
				" in " SHORT_PROGRAM_NAME " itself. The errors are as"
				" follows:</p>"
				"<ul>";
			end = internalFieldErrors.end();
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

			throw e.finalize();

		} else if (!config->genericApp && config->startsUsingWrapper) {
			session.journey.setStepErrored(SUBPROCESS_WRAPPER_PREPARATION);
			loadJourneyStateFromResponseDir();

			SpawnException e(
				INTERNAL_ERROR,
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			e.setAdvancedProblemDetails(toString(appSuppliedFieldErrors));
			loadAnnotationsFromEnvDumpDir(e);

			if (config->wrapperSuppliedByThirdParty) {
				e.setSummary("Error spawning the web application:"
					" a bug in a third-party application wrapper caused"
					" the spawn result to be invalid: "
					+ toString(appSuppliedFieldErrors));
			} else {
				e.setSummary("Error spawning the web application:"
					" a bug in a " SHORT_PROGRAM_NAME "-internal"
					" application wrapper caused the"
					" spawn result to be invalid: "
					+ toString(appSuppliedFieldErrors));
			}

			if (config->wrapperSuppliedByThirdParty) {
				message = "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through a helper tool "
					" called the \"wrapper\". This helper tool is not part of "
					SHORT_PROGRAM_NAME ". " SHORT_PROGRAM_NAME " expected"
					" the helper tool to communicate back various information"
					" about the application's startup sequence, but the tool"
					" did not communicate back correctly."
					" The errors are as follows:</p>"
					"<ul>";
			} else {
				message = "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through a " SHORT_PROGRAM_NAME
					"-internal helper tool (called the \"wrapper\"), "
					" but " SHORT_PROGRAM_NAME " encountered a bug"
					" in this helper tool. " SHORT_PROGRAM_NAME " expected"
					" the helper tool to communicate back various information"
					" about the application's startup sequence, but the tool"
					" did not communicate back correctly."
					" The errors are as follows:</p>"
					"<ul>";
			}
			end = appSuppliedFieldErrors.end();
			for (it = appSuppliedFieldErrors.begin(); it != end; it++) {
				message.append("<li>" + escapeHTML(*it) + "</li>");
			}
			message.append("</ul>");
			e.setProblemDescriptionHTML(message);

			if (config->wrapperSuppliedByThirdParty) {
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"This is a bug in the wrapper, so please contact the author of"
					" the wrapper. This problem is outside " SHORT_PROGRAM_NAME
					"'s control. Below follows the command that "
					SHORT_PROGRAM_NAME " tried to execute, so that you can infer"
					" which wrapper was used:</p>"
					"<pre>" + escapeHTML(config->startCommand) + "</pre>");
			} else {
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"This is a bug in " SHORT_PROGRAM_NAME "."
					" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
					" to the " SHORT_PROGRAM_NAME " authors.</p>");
			}

			throw e.finalize();

		} else {
			session.journey.setStepErrored(SUBPROCESS_APP_LOAD_OR_EXEC);
			loadJourneyStateFromResponseDir();

			SpawnException e(
				INTERNAL_ERROR,
				session.journey,
				config);
			e.setSummary("Error spawning the web application:"
				" the application's spawn response is invalid: "
				+ toString(appSuppliedFieldErrors));
			e.setAdvancedProblemDetails(toString(appSuppliedFieldErrors));
			e.setStdoutAndErrData(getStdoutErrData());
			loadAnnotationsFromEnvDumpDir(e);

			message = "<p>The " PROGRAM_NAME " application server tried"
				" to start the web application, but encountered a bug"
				" in the application. " SHORT_PROGRAM_NAME " expected"
				" the application to communicate back various information"
				" about its startup sequence, but the application"
				" did not communicate back that correctly."
				" The errors are as follows:</p>"
				"<ul>";
			end = appSuppliedFieldErrors.end();
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

			throw e.finalize();
		}
	}

	ErrorCategory inferErrorCategoryFromResponseDir(ErrorCategory defaultValue) const {
		if (fileExists(session.responseDir + "/error/category")) {
			string value = strip(readAll(session.responseDir + "/error/category"));
			ErrorCategory category = stringToErrorCategory(value);

			if (category == UNKNOWN_ERROR_CATEGORY) {
				SpawnException e(
					INTERNAL_ERROR,
					session.journey,
					config);
				e.setStdoutAndErrData(getStdoutErrData());
				loadAnnotationsFromEnvDumpDir(e);

				if (!config->genericApp && config->startsUsingWrapper) {
					if (config->wrapperSuppliedByThirdParty) {
						e.setSummary(
							"An error occurred while spawning an application process: "
							"the application wrapper (which is internal to "
							SHORT_PROGRAM_NAME
							") reported an invalid progress step state: "
							+ value);
					} else {
						e.setSummary(
							"An error occurred while spawning an application process: "
							"the application wrapper (which is not part of "
							SHORT_PROGRAM_NAME
							") reported an invalid progress step state: "
							+ value);
					}
				} else {
					e.setSummary(
						"An error occurred while spawning an application process: "
						"the application reported an invalid progress step state: "
						+ value);
				}

				if (!config->genericApp && config->startsUsingWrapper) {
					if (config->wrapperSuppliedByThirdParty) {
						e.setProblemDescriptionHTML(
							"<p>The " PROGRAM_NAME " application server tried"
							" to start the web application through a " SHORT_PROGRAM_NAME
							"-internal helper tool called the \"wrapper\". "
							" The tool encountered an error, so "
							SHORT_PROGRAM_NAME " expected the tool to report"
							" details about that error. But the tool communicated back"
							" in an invalid format:</p>"
							"<ul>"
							"<li>In file: " + escapeHTML(session.responseDir) + "/error/category</li>"
							"<li>Content: <code>" + escapeHTML(value) + "</code></li>"
							"</ul>");
						e.setSolutionDescriptionHTML(
							"<p class=\"sole-solution\">"
							"This is a bug in the wrapper, so please contact the author of"
							" the wrapper. This problem is outside " SHORT_PROGRAM_NAME
							"'s control. Below follows the command that "
							SHORT_PROGRAM_NAME " tried to execute, so that you can infer"
							" which wrapper was used:</p>"
							"<pre>" + escapeHTML(config->startCommand) + "</pre>");
					} else {
						e.setProblemDescriptionHTML(
							"<p>The " PROGRAM_NAME " application server tried"
							" to start the web application through a "
							" helper tool called the \"wrapper\". This helper tool "
							" is not part of " SHORT_PROGRAM_NAME ". The tool "
							" encountered an error, so " SHORT_PROGRAM_NAME
							" expected the tool to report details about that error."
							" But the tool communicated back in an invalid format:</p>"
							"<ul>"
							"<li>In file: " + escapeHTML(session.responseDir) + "/error/category</li>"
							"<li>Content: <code>" + escapeHTML(value) + "</code></li>"
							"</ul>");
						e.setSolutionDescriptionHTML(
							"<p class=\"sole-solution\">"
							"This is a bug in " SHORT_PROGRAM_NAME "."
							" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
							" to the " SHORT_PROGRAM_NAME " authors.</p>");
					}
				} else {
					e.setProblemDescriptionHTML(
						"<p>The " PROGRAM_NAME " application server tried"
						" to start the web application. The application encountered "
						" an error and tried to report details about the error back to "
						SHORT_PROGRAM_NAME ". But the application communicated back"
						" in an invalid format:</p>"
						"<ul>"
						"<li>In file: " + escapeHTML(session.responseDir) + "/error/category</li>"
							"<li>Content: <code>" + escapeHTML(value) + "</code></li>"
						"</ul>");
					e.setSolutionDescriptionHTML(
						"<p class=\"sole-solution\">"
						"This is a bug in the web application, please "
						"report this problem to the application's developer. "
						"This problem is outside " SHORT_PROGRAM_NAME "'s "
						"control.</p>");
				}

				throw e.finalize();
			} else {
				return category;
			}
		} else {
			return defaultValue;
		}
	}

	void loadJourneyStateFromResponseDir() {
		JourneyStep firstStep = JourneyStep((int) getFirstSubprocessJourneyStep() + 1);
		JourneyStep lastStep = getLastSubprocessJourneyStep();
		JourneyStep step;

		for (step = firstStep; step < lastStep; step = JourneyStep((int) step + 1)) {
			if (!session.journey.hasStep(step)) {
				continue;
			}

			string stepString = journeyStepToStringLowerCase(step);
			string stepDir = session.responseDir + "/steps/" + stepString;
			if (!fileExists(stepDir + "/state")) {
				continue;
			}

			loadJourneyStateFromResponseDirForSpecificStep(
				step, stepDir);
		}
	}

	void loadJourneyStateFromResponseDirForSpecificStep(JourneyStep step,
		const string &stepDir) const
	{
		string summary;
		string value = strip(readAll(stepDir + "/state"));
		JourneyStepState state = stringToJourneyStepState(value);

		if (session.journey.getStepInfo(step).state == state) {
			return;
		}

		try {
			switch (state) {
			case STEP_IN_PROGRESS:
				session.journey.setStepInProgress(step);
				break;
			case STEP_PERFORMED:
				session.journey.setStepPerformed(step);
				break;
			case STEP_ERRORED:
				session.journey.setStepErrored(step);
				break;
			default:
				session.journey.setStepErrored(step);

				SpawnException e(
					INTERNAL_ERROR,
					session.journey,
					config);
				e.setStdoutAndErrData(getStdoutErrData());
				loadAnnotationsFromEnvDumpDir(e);

				if (!config->genericApp && config->startsUsingWrapper) {
					if (config->wrapperSuppliedByThirdParty) {
						e.setSummary(
							"An error occurred while spawning an application process: "
							"the application wrapper (which is internal to " SHORT_PROGRAM_NAME
							") reported an invalid progress step state: "
							+ value);
					} else {
						e.setSummary(
							"An error occurred while spawning an application process: "
							"the application wrapper (which is not part of " SHORT_PROGRAM_NAME
							") reported an invalid progress step state: "
							+ value);
					}
				} else {
					e.setSummary(
						"An error occurred while spawning an application process: "
						"the application reported an invalid progress step state: "
						+ value);
				}

				if (!config->genericApp && config->startsUsingWrapper) {
					if (config->wrapperSuppliedByThirdParty) {
						e.setProblemDescriptionHTML(
							"<p>The " PROGRAM_NAME " application server tried"
							" to start the web application through a " SHORT_PROGRAM_NAME
							"-internal helper tool called the \"wrapper\", "
							" but " SHORT_PROGRAM_NAME " encountered a bug"
							" in this helper tool. " SHORT_PROGRAM_NAME " expected"
							" the helper tool to report about its startup progress,"
							" but the tool communicated back an invalid answer:</p>"
							"<ul>"
							"<li>In file: " + escapeHTML(stepDir) + "/state</li>"
							"<li>Content: <code>" + escapeHTML(value) + "</code></li>"
							"</ul>");
						e.setSolutionDescriptionHTML(
							"<p class=\"sole-solution\">"
							"This is a bug in the wrapper, so please contact the author of"
							" the wrapper. This problem is outside " SHORT_PROGRAM_NAME
							"'s control. Below follows the command that "
							SHORT_PROGRAM_NAME " tried to execute, so that you can infer"
							" which wrapper was used:</p>"
							"<pre>" + escapeHTML(config->startCommand) + "</pre>");
					} else {
						e.setProblemDescriptionHTML(
							"<p>The " PROGRAM_NAME " application server tried"
							" to start the web application through a "
							" helper tool called the \"wrapper\". This helper tool "
							" is not part of " SHORT_PROGRAM_NAME ". "
							SHORT_PROGRAM_NAME " expected the helper tool to"
							" report about its startup progress, but the tool"
							" communicated back an invalid answer:</p>"
							"<ul>"
							"<li>In file: " + escapeHTML(stepDir) + "/state</li>"
							"<li>Content: <code>" + escapeHTML(value) + "</code></li>"
							"</ul>");
						e.setSolutionDescriptionHTML(
							"<p class=\"sole-solution\">"
							"This is a bug in " SHORT_PROGRAM_NAME "."
							" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
							" to the " SHORT_PROGRAM_NAME " authors.</p>");
					}
				} else {
					e.setProblemDescriptionHTML(
						"<p>The " PROGRAM_NAME " application server tried"
						" to start the web application, and expected the application"
						" to report about its startup progress. But the application"
						" communicated back an invalid answer:</p>"
						"<ul>"
						"<li>In file: " + escapeHTML(stepDir) + "/state</li>"
						"<li>Content: <code>" + escapeHTML(value) + "</code></li>"
						"</ul>");
					e.setSolutionDescriptionHTML(
						"<p class=\"sole-solution\">"
						"This is a bug in the web application, please "
						"report this problem to the application's developer. "
						"This problem is outside " SHORT_PROGRAM_NAME "'s "
						"control.</p>");
				}

				throw e.finalize();
				break;
			};
		} catch (const RuntimeException &originalException) {
			session.journey.setStepErrored(step, true);

			SpawnException e(
				INTERNAL_ERROR,
				session.journey,
				config);
			e.setStdoutAndErrData(getStdoutErrData());
			loadAnnotationsFromEnvDumpDir(e);

			if (!config->genericApp && config->startsUsingWrapper) {
				if (config->wrapperSuppliedByThirdParty) {
					e.setSummary("An error occurred while spawning an application process: "
						"the application wrapper (which is internal to " SHORT_PROGRAM_NAME
						") reported an invalid progress step state: "
						+ StaticString(originalException.what()));
				} else {
					e.setSummary("An error occurred while spawning an application process: "
						"the application wrapper (which is not part of " SHORT_PROGRAM_NAME
						") reported an invalid progress step state: "
						+ StaticString(originalException.what()));
				}
			} else {
				e.setSummary("An error occurred while spawning an application process: "
					"the application reported an invalid progress step state: "
					+ StaticString(originalException.what()));
			}

			if (!config->genericApp && config->startsUsingWrapper) {
				if (config->wrapperSuppliedByThirdParty) {
					e.setProblemDescriptionHTML(
						"<p>The " PROGRAM_NAME " application server tried"
						" to start the web application through a " SHORT_PROGRAM_NAME
						"-internal helper tool called the \"wrapper\", "
						" but " SHORT_PROGRAM_NAME " encountered a bug"
						" in this helper tool. " SHORT_PROGRAM_NAME " expected"
						" the helper tool to report about its startup progress,"
						" but the tool communicated back an invalid answer:</p>"
						"<ul>"
						"<li>In file: " + escapeHTML(stepDir) + "/state</li>"
						"<li>Error: " + escapeHTML(originalException.what()) + "</li>"
						"</ul>");
					e.setSolutionDescriptionHTML(
						"<p class=\"sole-solution\">"
						"This is a bug in the wrapper, so please contact the author of"
						" the wrapper. This problem is outside " SHORT_PROGRAM_NAME
						"'s control. Below follows the command that "
						SHORT_PROGRAM_NAME " tried to execute, so that you can infer"
						" which wrapper was used:</p>"
						"<pre>" + escapeHTML(config->startCommand) + "</pre>");
				} else {
					e.setProblemDescriptionHTML(
						"<p>The " PROGRAM_NAME " application server tried"
						" to start the web application through a "
						" helper tool called the \"wrapper\". This helper tool "
						" is not part of " SHORT_PROGRAM_NAME ". "
						SHORT_PROGRAM_NAME " expected the helper tool to"
						" report about its startup progress, but the tool"
						" communicated back an invalid answer:</p>"
						"<ul>"
						"<li>In file: " + escapeHTML(stepDir) + "/state</li>"
						"<li>Error: " + escapeHTML(originalException.what()) + "</li>"
						"</ul>");
					e.setSolutionDescriptionHTML(
						"<p class=\"sole-solution\">"
						"This is a bug in " SHORT_PROGRAM_NAME "."
						" <a href=\"" SUPPORT_URL "\">Please report this bug</a>"
						" to the " SHORT_PROGRAM_NAME " authors.</p>");
				}
			} else {
				e.setProblemDescriptionHTML(
					"<p>The " PROGRAM_NAME " application server tried"
					" to start the web application, and expected the application"
					" to report about its startup progress. But the application"
					" communicated back an invalid answer:</p>"
					"<ul>"
					"<li>In file: " + escapeHTML(stepDir) + "/state</li>"
					"<li>Error: " + escapeHTML(originalException.what()) + "</li>"
					"</ul>");
				e.setSolutionDescriptionHTML(
					"<p class=\"sole-solution\">"
					"This is a bug in the web application, please "
					"report this problem to the application's developer. "
					"This problem is outside " SHORT_PROGRAM_NAME "'s "
					"control.</p>");
			}

			throw e.finalize();
		}

		if (fileExists(stepDir + "/duration")) {
			value = readAll(stepDir + "/duration");
			unsigned long long usecDuration = stringToULL(value) * 1000000;
			session.journey.setStepExecutionDuration(step, usecDuration);
		}
	}

	void loadSubprocessErrorMessagesAndAnnotations(SpawnException &e) const {
		const string &responseDir = session.responseDir;
		const string &envDumpDir = session.envDumpDir;

		if (fileExists(responseDir + "/error/summary")) {
			e.setSummary(strip(readAll(responseDir + "/error/summary")));
		}

		if (e.getAdvancedProblemDetails().empty()
		 && fileExists(responseDir + "/error/advanced_problem_details"))
		{
			e.setAdvancedProblemDetails(strip(readAll(responseDir
				+ "/error/advanced_problem_details")));
		}

		if (fileExists(responseDir + "/error/problem_description.html")) {
			e.setProblemDescriptionHTML(readAll(responseDir + "/error/problem_description.html"));
		} else if (fileExists(responseDir + "/error/problem_description.txt")) {
			e.setProblemDescriptionHTML(escapeHTML(strip(readAll(
				responseDir + "/error/problem_description.txt"))));
		}

		if (fileExists(responseDir + "/error/solution_description.html")) {
			e.setSolutionDescriptionHTML(readAll(responseDir + "/error/solution_description.html"));
		} else if (fileExists(responseDir + "/error/solution_description.txt")) {
			e.setSolutionDescriptionHTML(escapeHTML(strip(readAll(
				responseDir + "/error/solution_description.txt"))));
		}

		if (fileExists(envDumpDir + "/envvars")) {
			e.setSubprocessEnvvars(readAll(envDumpDir + "/envvars"));
		}
		if (fileExists(envDumpDir + "/user_info")) {
			e.setSubprocessUserInfo(readAll(envDumpDir + "/user_info"));
		}
		if (fileExists(envDumpDir + "/ulimits")) {
			e.setSubprocessUlimits(readAll(envDumpDir + "/ulimits"));
		}

		loadAnnotationsFromEnvDumpDir(e);
	}

	static void doClosedir(DIR *dir) {
		closedir(dir);
	}

	void loadAnnotationsFromEnvDumpDir(SpawnException &e) const {
		string path = session.envDumpDir + "/annotations";
		DIR *dir = opendir(path.c_str());
		if (dir == NULL) {
			return;
		}

		ScopeGuard guard(boost::bind(doClosedir, dir));
		struct dirent *ent;
		while ((ent = readdir(dir)) != NULL) {
			if (ent->d_name[0] != '.') {
				e.setAnnotation(ent->d_name, strip(
					Passenger::readAll(path + "/" + ent->d_name)));
			}
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

		session.journey.setStepInProgress(SPAWNING_KIT_HANDSHAKE_PERFORM);

		try {
			initializeStdchannelsCapturing();
			startWatchingProcessExit();
			if (config->genericApp || config->findFreePort) {
				startWatchingSocketPingability();
			}
			if (!config->genericApp) {
				startWatchingFinishSignal();
			}
		} catch (const SpawnException &) {
			throw;
		} catch (const std::exception &originalException) {
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.setStepErrored(SPAWNING_KIT_HANDSHAKE_PERFORM);
			loadJourneyStateFromResponseDir();
			SpawnException e(originalException, session.journey, config);
			e.setStdoutAndErrData(getStdoutErrData());
			throw e.finalize();
		}

		UPDATE_TRACE_POINT();
		try {
			boost::unique_lock<boost::mutex> l(syncher);
			waitUntilSpawningFinished(l);
			Result result = handleResponse();
			session.journey.setStepPerformed(SPAWNING_KIT_HANDSHAKE_PERFORM);
			loadJourneyStateFromResponseDir();
			return result;
		} catch (const SpawnException &) {
			throw;
		} catch (const std::exception &originalException) {
			sleepShortlyToCaptureMoreStdoutStderr();
			session.journey.setStepErrored(SPAWNING_KIT_HANDSHAKE_PERFORM);
			loadJourneyStateFromResponseDir();
			SpawnException e(originalException, session.journey, config);
			e.setStdoutAndErrData(getStdoutErrData());
			throw e.finalize();
		}
	}
};


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_HANDSHAKE_PERFORM_H_ */
