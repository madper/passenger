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
#ifndef _PASSENGER_SPAWNING_KIT_EXCEPTIONS_H_
#define _PASSENGER_SPAWNING_KIT_EXCEPTIONS_H_

#include <oxt/tracable_exception.hpp>
#include <string>
#include <stdexcept>

#include <Constants.h>
#include <Exceptions.h>
#include <Logging.h>
#include <DataStructures/StringKeyTable.h>
#include <Utils.h>
#include <Utils/StrIntUtils.h>
#include <Utils/SystemMetricsCollector.h>
#include <Core/SpawningKit/Config.h>
#include <Core/SpawningKit/Journey.h>

extern "C" {
	extern char **environ;
}

namespace Passenger {
namespace SpawningKit {

using namespace std;
using namespace oxt;


class SpawnException: public oxt::tracable_exception {
public:
	enum ErrorCategory {
		INTERNAL_ERROR,
		FILE_SYSTEM_ERROR,
		OPERATING_SYSTEM_ERROR,
		IO_ERROR,
		TIMEOUT_ERROR,

		UNKNOWN_ERROR_CATEGORY
	};

private:
	ErrorCategory category;
	Journey journey;
	Config config;

	string summary;
	string lowLevelErrorMessage;
	string stdoutAndErrData;
	string problemDescription;
	string solutionDescription;

	string envvars;
	string ulimits;
	string systemMetrics;
	StringKeyTable<string> annotations;

	static string createDefaultSummary(ErrorCategory category,
		const Journey &journey, const StaticString &lowLevelErrorMessage)
	{
		switch (category) {
		case TIMEOUT_ERROR:
			// We only return a single error message instead of a customized
			// one based on the failed step, because the timeout
			// applies to the entire journey, not just to a specific step.
			// A timeout at a specific step could be the result of a previous
			// step taking too much time.
			// The way to debug a timeout error is by looking at the timings
			// of each step.
			switch (journey.getType()) {
			case START_PRELOADER:
				return "A timeout occurred while preparing to start a preloader process.";
			default:
				return "A timeout occurred while spawning an application process";
			}
		default:
			string categoryPhraseWithIndefiniteArticle =
				getErrorCategoryPhraseWithIndefiniteArticle(
					category, true);
			switch (journey.getType()) {
			case START_PRELOADER:
				switch (journey.getFirstFailedStep()) {
				case SPAWNING_KIT_PREPARATION:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while preparing to start a preloader process: "
						+ lowLevelErrorMessage;
				default:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while starting a preloader process: "
						+ lowLevelErrorMessage;
				}
			default:
				switch (journey.getFirstFailedStep()) {
				case SPAWNING_KIT_PREPARATION:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while preparing to spawn an application process: "
						+ lowLevelErrorMessage;
				case SPAWNING_KIT_FORK_SUBPROCESS:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while creating (forking) subprocess: "
						+ lowLevelErrorMessage;
				case SPAWNING_KIT_CONNECT_TO_PRELOADER:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while connecting to the preloader process: "
						+ lowLevelErrorMessage;
				case SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while sending a command to the preloader process: "
						+ lowLevelErrorMessage;
				case SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while receiving a response from the preloader process: "
						+ lowLevelErrorMessage;
				case SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while parsing a response from the preloader process: "
						+ lowLevelErrorMessage;
				case SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while processing a response from the preloader process: "
						+ lowLevelErrorMessage;
				default:
					return categoryPhraseWithIndefiniteArticle
						+ " occurred while spawning an application process: "
						+ lowLevelErrorMessage;
				}
			}
		}
	}

	static string createDefaultProblemDescription(ErrorCategory category,
		const Journey &journey, const StaticString &lowLevelErrorMessage)
	{
		StaticString categoryStringWithIndefiniteArticle =
			getErrorCategoryPhraseWithIndefiniteArticle(category,
				false);

		switch (category) {
		case INTERNAL_ERROR:
		case OPERATING_SYSTEM_ERROR:
		case IO_ERROR:
			switch (journey.getType()) {
			case START_PRELOADER:
				switch (journey.getFirstFailedStep()) {
				case SPAWNING_KIT_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\". But "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle +
						" while performing this preparation work",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_FORK_SUBPROCESS:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. But " SHORT_PROGRAM_NAME
						" encountered " + categoryStringWithIndefiniteArticle
						+ " while creating a subprocess",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_HANDSHAKE_PERFORM:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " first started an internal"
						" helper tool called the \"preloader\". But "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle +
						" while communicating with"
						" this tool about its startup",
						category, lowLevelErrorMessage);
				case SUBPROCESS_BEFORE_FIRST_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\". But"
						" the subprocess which was supposed to execute this"
						" preloader encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_OS_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\", which"
						" in turn had to be started through the operating"
						" system (OS) shell. But the OS shell encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
				case SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\", which"
						" in turn had to be started through another internal"
						" tool called the \"SpawnEnvSetupper\". But the"
						" SpawnEnvSetupper encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_EXEC_WRAPPER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool called"
						" the \"wrapper\". But " SHORT_PROGRAM_NAME
						" was unable to execute that helper tool"
						" because it encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_WRAPPER_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool called"
						" the \"wrapper\"). But that helper tool encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_APP_LOAD_OR_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. But the application"
						" itself (and not " SHORT_PROGRAM_NAME ") encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_LISTEN:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. The application tried "
						" to setup a socket for accepting connections,"
						" but in doing so it encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				default:
					P_BUG("Unsupported preloader journey step "
						<< toString((int) journey.getFirstFailedStep()));
				}
			default:
				switch (journey.getFirstFailedStep()) {
				case SPAWNING_KIT_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application, but " SHORT_PROGRAM_NAME
						" encountered " + categoryStringWithIndefiniteArticle
						+ " while performing preparation work",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_FORK_SUBPROCESS:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. But " SHORT_PROGRAM_NAME
						" encountered " + categoryStringWithIndefiniteArticle
						+ " while creating a subprocess",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_CONNECT_TO_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle
						+ " while connecting to this helper process",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle
						+ " while sending a command to this helper process",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle
						+ " while receiving a response to this helper process",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle
						+ " while parsing a response from this helper process",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle
						+ " while processing a response from this helper process",
						category, lowLevelErrorMessage);
				case SPAWNING_KIT_HANDSHAKE_PERFORM:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. Everything was looking OK,"
						" but then suddenly " SHORT_PROGRAM_NAME " encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_BEFORE_FIRST_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. " SHORT_PROGRAM_NAME
						" launched a subprocess which was supposed to"
						" execute the application, but instead that"
						" subprocess encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_OS_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through the operating"
						" system (OS) shell. But the OS shell encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
				case SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool called the"
						" SpawnEnvSetupper. But that helper tool encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_EXEC_WRAPPER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool called"
						" the \"wrapper\". But " SHORT_PROGRAM_NAME
						" was unable to execute that helper tool because"
						" it encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_WRAPPER_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool called"
						" the \"wrapper\". But that helper tool encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_APP_LOAD_OR_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. But the application"
						" itself (and not " SHORT_PROGRAM_NAME ") encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool called"
						" the \"wrapper\". But the preloader encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				case SUBPROCESS_LISTEN:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. The application tried "
						" to setup a socket for accepting connections,"
						" but in doing so it encountered "
						+ categoryStringWithIndefiniteArticle,
						category, lowLevelErrorMessage);
				default:
					P_BUG("Unrecognized journey step " <<
						toString((int) journey.getFirstFailedStep()));
				}
			}

		case TIMEOUT_ERROR:
			// We only return a single error message instead of a customized
			// one based on the failed step, because the timeout
			// applies to the entire journey, not just to a specific step.
			// A timeout at a specific step could be the result of a previous
			// step taking too much time.
			// The way to debug a timeout error is by looking at the timings
			// of each step.
			return "<p>The " PROGRAM_NAME " application server tried"
				" to start the web application, but this took too much time,"
				" so " SHORT_PROGRAM_NAME " put a stop to that.</p>";

		default:
			P_BUG("Unrecognized error category " + toString((int) category));
			return string(); // Never reached, shut up compiler warning.
		}
	}

	static string createDefaultSolutionDescription(ErrorCategory category,
		const Journey &journey, const Config &config)
	{
		string message;

		switch (category) {
		case INTERNAL_ERROR:
			return "<p class=\"sole-solution\">"
				"Unfortunately, " SHORT_PROGRAM_NAME " does not know"
				" how to solve this problem. Please try troubleshooting"
				" the problem by studying the <strong>error message</strong>"
				" and the <strong>diagnostics</strong> reports. You can also"
				" consult <a href=\"" SUPPORT_URL "\">the " SHORT_PROGRAM_NAME
				" support resources</a> for help.</p>";

		case FILE_SYSTEM_ERROR:
			return "<p class=\"sole-solution\">"
				"Unfortunately, " SHORT_PROGRAM_NAME " does not know how to"
				" solve this problem. But it looks like some kind of filesystem error."
				" This generally means that you need to fix nonexistant"
				" files/directories or fix filesystem permissions. Please"
				" try troubleshooting the problem by studying the"
				" <strong>error message</strong> and the"
				" <strong>diagnostics</strong> reports.</p>";

		case OPERATING_SYSTEM_ERROR:
		case IO_ERROR:
			return "<div class=\"multiple-solutions\">"

				"<h3>Check whether the server is low on resources</h3>"
				"<p>Maybe the server is currently low on resources. This would"
				" cause errors to occur. Please study the <em>error"
				" message</em> and the <em>diagnostics reports</em> to"
				" verify whether this is the case. Key things to check for:</p>"
				"<ul>"
				"<li>Excessive CPU usage</li>"
				"<li>Memory and swap</li>"
				"<li>Ulimits</li>"
				"</ul>"
				"<p>If the server is indeed low on resources, find a way to"
				" free up some resources.</p>"

				"<h3>Check your (filesystem) security settings</h3>"
				"<p>Maybe security settings are preventing " SHORT_PROGRAM_NAME
				" from doing the work it needs to do. Please check whether the"
				" error may be caused by your system's security settings, or"
				" whether it may be caused by wrong permissions on a file or"
				" directory.</p>"

				"<h3>Still no luck?</h3>"
				"<p>Please try troubleshooting the problem by studying the"
				" <em>diagnostics</em> reports.</p>"

				"</div>";

		case TIMEOUT_ERROR:
			message = "<div class=\"multiple-solutions\">"

				"<h3>Check whether the server is low on resources</h3>"
				"<p>Maybe the server is currently so low on resources that"
				" all the work that needed to be done, could not finish within"
				" the given time limit."
				" Please inspect the server resource utilization statistics"
				" in the <em>diagnostics</em> section to verify"
				" whether server is indeed low on resources.</p>"
				"<p>If so, then either increase the spawn timeout (currently"
				" configured at " + toString(config.startTimeoutMsec / 1000)
				+ " sec), or find a way to lower the server's resource"
				" utilization.</p>";

			switch (journey.getFirstFailedStep()) {
			case SUBPROCESS_OS_SHELL:
				message.append(
					"<h3>Check whether your OS shell's startup scripts can"
					" take a long time or get stuck</h3>"
					"<p>One of your OS shell's startup scripts may do too much work,"
					" or it may have invoked a command that then got stuck."
					" Please investigate and debug your OS shell's startup"
					" scripts.</p>");
				break;
			case SUBPROCESS_APP_LOAD_OR_EXEC:
				if (config.appType == "node") {
					message.append(
						"<h3>Check whether the application calls <code>http.Server.listen()</code></h3>"
						"<p>" SHORT_PROGRAM_NAME " requires that the application calls"
							" <code>listen()</code> on an http.Server object. If"
							" the application never calls this, then "
							SHORT_PROGRAM_NAME " will think the application is"
							" stuck. <a href=\"https://www.phusionpassenger.com/"
							"library/indepth/nodejs/reverse_port_binding.html\">"
							"Learn more about this problem.</a></p>");
				}
				message.append(
					"<h3>Check whether the application is stuck during startup</h3>"
					"<p>The easiest way find out where the application is stuck"
					"is by inserting print statements into the application's code.</p>");
				break;
			default:
				break;
			}

			message.append("<h3>Still no luck?</h3>"
				"<p>Please try troubleshooting the problem by studying the"
				" <em>diagnostics</em> reports.</p>"

				"</div>");
			return message;

		default:
			return "(error generating solution description: unknown error category)";
		}
	}

	static bool isFileSystemError(const std::exception &e) {
		if (dynamic_cast<const FileSystemException *>(&e) != NULL) {
			return true;
		}

		const SystemException *sysEx = dynamic_cast<const SystemException *>(&e);
		if (sysEx != NULL) {
			return sysEx->code() == ENOENT
				|| sysEx->code() == ENAMETOOLONG
				|| sysEx->code() == EEXIST
				|| sysEx->code() == EACCES;
		}

		return false;
	}

	static bool systemErrorIsActuallyIoError(JourneyStep failedJourneyStep) {
		return failedJourneyStep == SPAWNING_KIT_CONNECT_TO_PRELOADER
			|| failedJourneyStep == SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER
			|| failedJourneyStep == SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER;
	}

	static ErrorCategory inferErrorCategoryFromAnotherException(
		const std::exception &e, JourneyStep failedJourneyStep)
	{
		if (dynamic_cast<const SystemException *>(&e) != NULL) {
			if (systemErrorIsActuallyIoError(failedJourneyStep)) {
				return IO_ERROR;
			} else {
				return OPERATING_SYSTEM_ERROR;
			}
		} else if (isFileSystemError(e)) {
			return FILE_SYSTEM_ERROR;
		} else if (dynamic_cast<const IOException *>(&e) != NULL) {
			return IO_ERROR;
		} else if (dynamic_cast<const TimeoutException *>(&e) != NULL) {
			return TIMEOUT_ERROR;
		} else {
			return INTERNAL_ERROR;
		}
	}

	static StaticString getErrorCategoryPhraseWithIndefiniteArticle(
		ErrorCategory category, bool beginOfSentence)
	{
		switch (category) {
		case INTERNAL_ERROR:
			if (beginOfSentence) {
				return P_STATIC_STRING("An internal error");
			} else {
				return P_STATIC_STRING("an internal error");
			}
		case FILE_SYSTEM_ERROR:
			if (beginOfSentence) {
				return P_STATIC_STRING("A file system error");
			} else {
				return P_STATIC_STRING("a file system error");
			}
		case OPERATING_SYSTEM_ERROR:
			if (beginOfSentence) {
				return P_STATIC_STRING("An operating system error");
			} else {
				return P_STATIC_STRING("an operating system error");
			}
		case IO_ERROR:
			if (beginOfSentence) {
				return P_STATIC_STRING("An I/O error");
			} else {
				return P_STATIC_STRING("an I/O error");
			}
		default:
			P_BUG("Unsupported error category " + toString((int) category));
			return StaticString();
		}
	}

	static StaticString getErrorCategoryPhraseWithIndefiniteArticle(
		const std::exception &e, const Journey &journey,
		bool beginOfSentence)
	{
		ErrorCategory category =
			inferErrorCategoryFromAnotherException(
				e, journey.getFirstFailedStep());
		return getErrorCategoryPhraseWithIndefiniteArticle(category, beginOfSentence);
	}

	static string wrapInParaAndMaybeAddErrorMessage(const string &message,
		ErrorCategory category, const string &lowLevelErrorMessage)
	{
		if (lowLevelErrorMessage.empty()) {
			return "<p>" + message + ".</p>";
		} else if (category == INTERNAL_ERROR || category == FILE_SYSTEM_ERROR) {
			return "<p>" + message + ":</p>" +
				"<pre>" + escapeHTML(lowLevelErrorMessage) + "</pre>";
		} else if (category == IO_ERROR) {
			return "<p>" + message
				+ ". The error reported by the I/O layer is:</p>" +
				"<pre>" + escapeHTML(lowLevelErrorMessage) + "</pre>";
		} else {
			P_ASSERT_EQ(category, OPERATING_SYSTEM_ERROR);
			return "<p>" + message
				+ ". The error reported by the operating system is:</p>" +
				"<pre>" + escapeHTML(lowLevelErrorMessage) + "</pre>";
		}
	}

	static string gatherEnvvars() {
		string result;

		unsigned int i = 0;
		while (environ[i] != NULL) {
			result.append(environ[i]);
			result.append(1, '\n');
			i++;
		}

		return result;
	}

	static string gatherUlimits() {
		const char *command[] = { "ulimit", "-a", NULL };
		return runCommandAndCaptureOutput(command);
	}

	static string gatherSystemMetrics() {
		SystemMetrics metrics;

		try {
			SystemMetricsCollector().collect(metrics);
		} catch (const RuntimeException &e) {
			return "Error: cannot parse system metrics: " + StaticString(e.what());
		}

		FastStringStream<> stream;
		metrics.toDescription(stream);
		return string(stream.data(), stream.size());
	}

public:
	SpawnException(ErrorCategory _category, const Journey &_journey,
		const Config *_config, const string &summary,
		const string &_lowLevelErrorMessage = string(),
		const string &_stdoutAndErrData = string())
		: category(_category),
		  journey(_journey),
		  config(*_config),
		  summary(summary),
		  lowLevelErrorMessage(_lowLevelErrorMessage),
		  stdoutAndErrData(_stdoutAndErrData)
	{
		assert(_journey.getFirstFailedStep() != UNKNOWN_JOURNEY_STEP);
		config.internStrings();
	}

	SpawnException(const std::exception &originalException,
		const Journey &_journey, const Config *_config,
		const string &_stdoutAndErrData = string())
		: category(inferErrorCategoryFromAnotherException(
		      originalException, _journey.getFirstFailedStep())),
		  journey(_journey),
		  config(*_config),
		  summary(createDefaultSummary(
		      category, _journey, originalException.what())),
		  lowLevelErrorMessage(originalException.what()),
		  stdoutAndErrData(_stdoutAndErrData)
	{
		assert(_journey.getFirstFailedStep() != UNKNOWN_JOURNEY_STEP);
		config.internStrings();
	}

	virtual ~SpawnException() throw() {}

	const string &getProblemDescriptionHTML() const {
		return problemDescription;
	}

	void setProblemDescriptionHTML(const string &value) {
		problemDescription = value;
	}

	const string &getSolutionDescriptionHTML() const {
		return solutionDescription;
	}

	void setSolutionDescriptionHTML(const string &value) {
		solutionDescription = value;
	}

	SpawnException &finalize() {
		if (problemDescription.empty()) {
			problemDescription = createDefaultProblemDescription(
				category, journey, lowLevelErrorMessage);
		}
		if (solutionDescription.empty()) {
			solutionDescription = createDefaultSolutionDescription(
				category, journey, config);
		}
		if (envvars.empty()) {
			envvars = gatherEnvvars();
		}
		if (ulimits.empty()) {
			ulimits = gatherUlimits();
		}
		if (systemMetrics.empty()) {
			systemMetrics = gatherSystemMetrics();
		}
		return *this;
	}


	virtual const char *what() const throw() {
		return summary.c_str();
	}

	ErrorCategory getErrorCategory() const {
		return category;
	}

	const Journey &getJourney() const {
		return journey;
	}

	const Config &getConfig() const {
		return config;
	}


	const string &getStdouterrData() const {
		return stdoutAndErrData;
	}

	const string &getEnvvars() const {
		return envvars;
	}

	const string &getUlimits() const {
		return ulimits;
	}

	const string &getSystemMetrics() const {
		return systemMetrics;
	}
};


inline StaticString
errorCategoryToString(SpawnException::ErrorCategory category) {
	switch (category) {
	case SpawnException::INTERNAL_ERROR:
		return P_STATIC_STRING("INTERNAL_ERROR");
	case SpawnException::FILE_SYSTEM_ERROR:
		return P_STATIC_STRING("FILE_SYSTEM_ERROR");
	case SpawnException::OPERATING_SYSTEM_ERROR:
		return P_STATIC_STRING("OPERATING_SYSTEM_ERROR");
	case SpawnException::IO_ERROR:
		return P_STATIC_STRING("IO_ERROR");
	case SpawnException::TIMEOUT_ERROR:
		return P_STATIC_STRING("TIMEOUT_ERROR");

	case SpawnException::UNKNOWN_ERROR_CATEGORY:
		return P_STATIC_STRING("UNKNOWN_ERROR_CATEGORY");

	default:
		return P_STATIC_STRING("(invalid value)");
	}
}


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_EXCEPTIONS_H_ */
