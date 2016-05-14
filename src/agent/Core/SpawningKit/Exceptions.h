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
#include <cerrno>

#include <dirent.h>

#include <Constants.h>
#include <Exceptions.h>
#include <Logging.h>
#include <DataStructures/StringKeyTable.h>
#include <Utils/StrIntUtils.h>
#include <Utils/IOUtils.h>
#include <Utils/ScopeGuard.h>
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
	enum ErrorKind {
		INTERNAL_ERROR,
		OPERATING_SYSTEM_ERROR,
		IO_ERROR,
		TIMEOUT_ERROR,

		UNKNOWN_ERROR_KIND,
		DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR
	};

private:
	Journey journey;
	ErrorKind errorKind;
	string summary;
	string problemDescription;
	string solutionDescription;
	string envvars;
	string ulimits;
	string systemMetrics;
	string stdoutAndErrData;
	Config config;
	StringKeyTable<string> annotations;

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
		return failedJourneyStep == PASSENGER_CORE_CONNECT_TO_PRELOADER
			|| failedJourneyStep == PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER
			|| failedJourneyStep == PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER;
	}

	static ErrorKind inferErrorKindFromAnotherException(const std::exception &e,
		JourneyStep failedJourneyStep)
	{
		if (dynamic_cast<const SystemException *>(&e) != NULL) {
			if (systemErrorIsActuallyIoError(failedJourneyStep)) {
				return IO_ERROR;
			} else {
				return OPERATING_SYSTEM_ERROR;
			}
		} else if (dynamic_cast<const IOException *>(&e) != NULL) {
			return IO_ERROR;
		} else if (dynamic_cast<const TimeoutException *>(&e) != NULL) {
			return TIMEOUT_ERROR;
		} else {
			return INTERNAL_ERROR;
		}
	}

	static string createDefaultSummary(const std::exception &originalException,
		const Journey &journey)
	{
		if (dynamic_cast<const TimeoutException *>(&originalException) != NULL) {
			if (journey.type == JOURNEY_TYPE_START_PRELOADER) {
				switch (journey.failedStep) {
				case PASSENGER_CORE_PREPARATION:
					return "A timeout occurred while preparing to start a preloader process.";
				default:
					return "A timeout occurred while starting a preloader process.";
				}
			} else {
				switch (journey.failedStep) {
				case PASSENGER_CORE_PREPARATION:
					return "A timeout occurred while preparing to spawn an application process.";
				case PASSENGER_CORE_CONNECT_TO_PRELOADER:
					return "A timeout occurred while connecting to the preloader process.";
				case PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER:
					return "A timeout occurred while sending a command to the preloader process.";
				case PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER:
					return "A timeout occurred while reading a response from the preloader process.";
				case PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER:
					return "A timeout occurred while parsing a response from the preloader process.";
				case PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER:
					return "A timeout occurred while processing a response from the preloader process.";
				default:
					return "A timeout occurred while spawning an application process.";
				}
			}
		} else {
			string errorKindPhraseWithIndefiniteArticle =
					getErrorKindPhraseWithIndefiniteArticle(originalException, journey);
			if (journey.type == JOURNEY_TYPE_START_PRELOADER) {
				switch (journey.failedStep) {
				case PASSENGER_CORE_PREPARATION:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while preparing to start a preloader process: "
						+ StaticString(originalException.what());
				default:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while starting a preloader process: "
						+ StaticString(originalException.what());
				}
			} else {
				switch (journey.failedStep) {
				case PASSENGER_CORE_PREPARATION:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while preparing to spawn an application process: "
						+ StaticString(originalException.what());
				case PASSENGER_CORE_CONNECT_TO_PRELOADER:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while connecting to the preloader process: "
						+ StaticString(originalException.what());
				case PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while sending a command to the preloader process: "
						+ StaticString(originalException.what());
				case PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while receiving a response from the preloader process: "
						+ StaticString(originalException.what());
				case PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while parsing a response from the preloader process: "
						+ StaticString(originalException.what());
				case PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while processing a response from the preloader process: "
						+ StaticString(originalException.what());
				default:
					return errorKindPhraseWithIndefiniteArticle
						+ " occurred while spawning an application process: "
						+ StaticString(originalException.what());
				}
			}
		}
	}

	static StaticString getErrorKindPhraseWithIndefiniteArticle(const std::exception &e,
		const Journey &journey)
	{
		if (dynamic_cast<const IOException *>(&e) != NULL) {
			return P_STATIC_STRING("An I/O error");
		} else if (dynamic_cast<const SystemException *>(&e) != NULL) {
			if (systemErrorIsActuallyIoError(journey.failedStep)) {
				return P_STATIC_STRING("An I/O error");
			} else {
				return P_STATIC_STRING("An error");
			}
		} else {
			return P_STATIC_STRING("An error");
		}
	}

	static string wrapInParaAndMaybeAddErrorMessage(const string &message,
		ErrorKind errorKind, const string &lowLevelErrorMessage)
	{
		if (lowLevelErrorMessage.empty()) {
			return "<p>" + message + ".</p>";
		} else if (errorKind == INTERNAL_ERROR) {
			return "<p>" + message + ":</p>" +
				"<pre>" + escapeHTML(lowLevelErrorMessage) + "</pre>";
		} else if (errorKind == IO_ERROR) {
			return "<p>" + message
				+ ". The error reported by the I/O layer is:</p>" +
				"<pre>" + escapeHTML(lowLevelErrorMessage) + "</pre>";
		} else {
			P_ASSERT_EQ(errorKind, OPERATING_SYSTEM_ERROR);
			return "<p>" + message
				+ ". The error reported by the operating system is:</p>" +
				"<pre>" + escapeHTML(lowLevelErrorMessage) + "</pre>";
		}
	}

	static string createDefaultProblemDescription(const Config *config,
		const Journey &journey, ErrorKind errorKind,
		const string &lowLevelErrorMessage = string())
	{
		StaticString errorKindString;

		switch (errorKind) {
		case INTERNAL_ERROR:
		case OPERATING_SYSTEM_ERROR:
		case IO_ERROR:
			switch (errorKind) {
			case INTERNAL_ERROR:
				errorKindString = P_STATIC_STRING("internal error");
				break;
			case OPERATING_SYSTEM_ERROR:
				errorKindString = P_STATIC_STRING("operating system error");
				break;
			case IO_ERROR:
				errorKindString = P_STATIC_STRING("I/O error");
				break;
			default:
				P_BUG("Unsupported errorKind " + toString((int) errorKind));
				break;
			}

			if (journey.type == JOURNEY_TYPE_START_PRELOADER) {
				switch (journey.failedStep) {
				case PASSENGER_CORE_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\". But "
						SHORT_PROGRAM_NAME " encountered an "
						+ errorKindString + " while performing preparation"
						" work",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_HANDSHAKE_PERFORM:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " first started an internal"
						" helper tool called the \"preloader\". But "
						SHORT_PROGRAM_NAME " encountered an "
						+ errorKindString + " while communicating with"
						" this tool about its startup",
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_BEFORE_FIRST_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\". But"
						" the subprocess which was supposed to execute this"
						" preloader encountered an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_OS_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\", which"
						" in turn had to be started through the operating"
						" system (OS) shell. But the OS shell encountered"
						" an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
				case SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\", which"
						" in turn had to be started through another internal"
						" tool called the \"SpawnEnvSetupper\". But the"
						" SpawnEnvSetupper encountered an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_WRAPPER_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. In doing so, "
						SHORT_PROGRAM_NAME " had to first start an internal"
						" helper tool called the \"preloader\". But this"
						" preloader encountered an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_APP_LOAD_OR_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. But the application"
						" itself (and not " SHORT_PROGRAM_NAME ") encountered"
						" an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				default:
					P_BUG("Unsupported preloader journeyStep "
						+ toString((int) journey.failedStep));
				}
			} else {
				switch (journey.failedStep) {
				case PASSENGER_CORE_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application, but " SHORT_PROGRAM_NAME
						" encountered an " + errorKindString + " while performing"
						" preparation work",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_CONNECT_TO_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered an " + errorKindString
						+ " while connecting to this helper process",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered an " + errorKindString
						+ " while sending a command to this helper process",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered an " + errorKindString
						+ " while receiving a response to this helper process",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered an " + errorKindString
						+ " while parsing a response from this helper process",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application by communicating with a"
						" helper process that we call a \"preloader\". However, "
						SHORT_PROGRAM_NAME " encountered an " + errorKindString
						+ " while processing a response from this helper process",
						errorKind, lowLevelErrorMessage);
				case PASSENGER_CORE_HANDSHAKE_PERFORM:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. Everything was looking OK,"
						" but then suddenly " SHORT_PROGRAM_NAME " encountered"
						" an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_BEFORE_FIRST_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. " SHORT_PROGRAM_NAME
						" launched a subprocess which was supposed to"
						" execute the application, but instead that"
						" subprocess encountered an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_OS_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through the operating"
						" system (OS) shell. But the OS shell encountered"
						" an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
				case SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool (in "
						" technical terms, called the SpawnEnvSetupper)."
						" But that helper tool encountered an "
						+ errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_WRAPPER_PREPARATION:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application through a "
						SHORT_PROGRAM_NAME "-internal helper tool (in"
						" technical terms, called the \"wrapper\"). But that"
						" helper tool encountered an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				case SUBPROCESS_APP_LOAD_OR_EXEC:
					return wrapInParaAndMaybeAddErrorMessage(
						"The " PROGRAM_NAME " application server tried to"
						" start the web application. But the application"
						" itself (and not " SHORT_PROGRAM_NAME ") encountered"
						" an " + errorKindString,
						errorKind, lowLevelErrorMessage);
				default:
					P_BUG("Unrecognized journeyStep " + toString((int) journey.failedStep));
				}
			}

		case TIMEOUT_ERROR:
			switch (journey.failedStep) {
			case PASSENGER_CORE_PREPARATION:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application, but the preparation"
					" work needed to this took too much time, so "
					SHORT_PROGRAM_NAME " put a stop to that.</p>";
			case PASSENGER_CORE_CONNECT_TO_PRELOADER:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However, "
					SHORT_PROGRAM_NAME " was unable to connect to this helper"
					" process within the time limit.</p>";
			case PASSENGER_CORE_SEND_COMMAND_TO_PRELOADER:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However, "
					SHORT_PROGRAM_NAME " was unable to send a command to"
					" this helper process within the time limit.</p>";
			case PASSENGER_CORE_READ_RESPONSE_FROM_PRELOADER:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However, "
					" this helper process took too much time sending a"
					" response.</p>";
			case PASSENGER_CORE_PARSE_RESPONSE_FROM_PRELOADER:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However, "
					SHORT_PROGRAM_NAME " was unable to parse the response"
					" from this helper process within the time limit.</p>";
			case PASSENGER_CORE_PROCESS_RESPONSE_FROM_PRELOADER:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application by communicating with a"
					" helper process that we call a \"preloader\". However, "
					SHORT_PROGRAM_NAME " was unable to process the response"
					" from this helper process within the time limit.</p>";
			case PASSENGER_CORE_HANDSHAKE_PERFORM:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application, but this took too"
					" much time, so " SHORT_PROGRAM_NAME " put a stop to"
					" that.</p>";
			case SUBPROCESS_BEFORE_FIRST_EXEC:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application. " SHORT_PROGRAM_NAME
					" launched a subprocess which was supposed to"
					" execute the application, but that subprocess took"
					" too much time doing that and didn't even begin to"
					" execute the application. So " SHORT_PROGRAM_NAME
					" put a stop to it.</p>";
			case SUBPROCESS_OS_SHELL:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through the operating"
					" system (OS) shell. But the OS shell took too much"
					" time to run or got stuck. So " SHORT_PROGRAM_NAME
					" put a stop to it.</p>";
			case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through a "
					SHORT_PROGRAM_NAME "-internal helper tool (in "
					" technical terms, called the SpawnEnvSetupper)."
					" But that helper tool took too much time, so "
					SHORT_PROGRAM_NAME " put a stop to it.</p>";
			case SUBPROCESS_WRAPPER_PREPARATION:
				return "<p>The " PROGRAM_NAME " application server tried"
					" to start the web application through a "
					SHORT_PROGRAM_NAME "-internal helper tool (in"
					" technical terms, called the \"wrapper\"). But that"
					" helper tool took too much time, so "
					SHORT_PROGRAM_NAME " put a stop to it.</p>";
			case SUBPROCESS_APP_LOAD_OR_EXEC:
				if (config->appType == "node") {
					return "<p>The " PROGRAM_NAME " application server tried"
						" to start the web application. But the application"
						" itself (and not " SHORT_PROGRAM_NAME ") took too"
						" much time to start, got stuck, or never told "
						SHORT_PROGRAM_NAME " that it is done starting."
						" So " SHORT_PROGRAM_NAME " put a stop to it.</p>";
				} else {
					return "<p>The " PROGRAM_NAME " application server tried"
						" to start the web application. But the application"
						" itself (and not " SHORT_PROGRAM_NAME ") took too"
						" much time to start or got stuck."
						" So " SHORT_PROGRAM_NAME " put a stop to it.</p>";
				}
			default:
				P_BUG("Unrecognized journeyStep " + toString((int) journey.failedStep));
				return string(); // Never reached, shut up compiler warning.
			}

		default:
			P_BUG("Unrecognized errorKind " + toString((int) errorKind));
			return string(); // Never reached, shut up compiler warning.
		}
	}

	static string createDefaultSolutionDescription(const Config *config,
		JourneyStep failedJourneyStep, ErrorKind errorKind,
		const std::exception *originalException = NULL)
	{
		string message;

		switch (errorKind) {
		case INTERNAL_ERROR:
			return "<p class=\"sole-solution\">"
				"Unfortunately, " SHORT_PROGRAM_NAME " does not know"
				" how to solve this problem. Please try troubleshooting"
				" the problem by studying the <strong>error message</strong>"
				" and the <strong>diagnostics</strong> reports. You can also"
				" consult <a href=\"" SUPPORT_URL "\">the " SHORT_PROGRAM_NAME
				" support resources</a> for help.</p>";

		case OPERATING_SYSTEM_ERROR:
		case IO_ERROR:
			if ((originalException != NULL && isFileSystemError(*originalException))) {
				return "<p class=\"sole-solution\">"
					"Unfortunately, " SHORT_PROGRAM_NAME " does not know how to"
					" solve this problem. But it looks like some kind of filesystem error."
					" This generally means that you need to fix nonexistant"
					" files/directories or fix filesystem permissions. Please"
					" try troubleshooting the problem by studying the"
					" <strong>error message</strong> and the"
					" <strong>diagnostics</strong> reports.</p>";
			} else {
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
			}

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
				" configured at " + toString(config->startTimeoutMsec / 1000)
				+ " sec), or find a way to lower the server's resource"
				" utilization.</p>";

			switch (failedJourneyStep) {
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
				if (config->appType == "node") {
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
			return "(error generating solution description: unknown error kind)";
		}
	}

	static string createProblemDescriptionFromPlainText(const StaticString &message) {
		return "<pre class=\"plain\">" + escapeHTML(message) + "</pre>";
	}

	static string createSolutionDescriptionFromPlainText(const StaticString &message) {
		return "<div class=\"sole-solution\">"
			"<pre class=\"plain\">" + escapeHTML(message) + "</pre>"
			"</div>";
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

	static void doClosedir(DIR *dir) {
		closedir(dir);
	}

	static ErrorKind stringToErrorKind(const StaticString &name) {
		// We only support a limited number of values for security reasons.
		// We don't want subprocesses to e.g. spoof DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR.
		if (name == P_STATIC_STRING("INTERNAL_ERROR")) {
			return INTERNAL_ERROR;
		} else if (name == P_STATIC_STRING("OPERATING_SYSTEM_ERROR")) {
			return OPERATING_SYSTEM_ERROR;
		} else if (name == P_STATIC_STRING("IO_ERROR")) {
			return IO_ERROR;
		} else if (name == P_STATIC_STRING("TIMEOUT_ERROR")) {
			return TIMEOUT_ERROR;
		} else {
			return UNKNOWN_ERROR_KIND;
		}
	}

	void loadInfoFromResponseDir(const string &responseDir,
		bool loadExceptionInfoFromResponseDir,
		bool determineErrorKindFromResponseDir)
	{
		DIR *dir = opendir(responseDir.c_str());
		ScopeGuard guard(boost::bind(doClosedir, dir));
		struct dirent *ent;
		string *message;

		while ((ent = readdir(dir)) != NULL) {
			if (ent->d_name[0] != '.') {
				try {
					annotations.insert(ent->d_name,
						Passenger::readAll(responseDir + "/" + ent->d_name));
				} catch (const SystemException &) {
					// Do nothing.
				}
			}
		}

		if (loadExceptionInfoFromResponseDir) {
			if (annotations.lookup("error_summary", &message)) {
				summary = *message;
			}
			if (annotations.lookup("error_problem_description.html", &message)) {
				problemDescription = *message;
			} else if (annotations.lookup("error_problem_description.txt", &message)) {
				problemDescription = createProblemDescriptionFromPlainText(*message);
			}
			if (annotations.lookup("error_solution_description.html", &message)) {
				solutionDescription = *message;
			} else if (annotations.lookup("error_solution_description.txt", &message)) {
				solutionDescription = createSolutionDescriptionFromPlainText(*message);
			}
			if (annotations.lookup("envvars", &message)) {
				envvars = *message;
			}
			if (annotations.lookup("ulimits", &message)) {
				ulimits = *message;
			}

			if (determineErrorKindFromResponseDir) {
				if (annotations.lookup("error_kind", &message)) {
					errorKind = stringToErrorKind(*message);
				} else {
					errorKind = INTERNAL_ERROR;
				}
			}
		}

		annotations.erase("error_summary");
		annotations.erase("error_problem_description.html");
		annotations.erase("error_problem_description.txt");
		annotations.erase("error_solution_description.html");
		annotations.erase("error_solution_description.txt");
		annotations.erase("envvars");
		annotations.erase("ulimits");
		annotations.erase("error_kind");
	}

public:
	SpawnException(const string &defaultSummary, const Config *_config,
		const Journey &_journey, ErrorKind defaultErrorKind,
		const string &lowLevelErrorMessage = string(),
		const string &_stdoutAndErrData = string(),
		const string &responseDir = string(),
		bool loadExceptionInfoFromResponseDir = false)
		: journey(_journey),
		  errorKind(defaultErrorKind),
		  summary(defaultSummary),
		  systemMetrics(gatherSystemMetrics()),
		  stdoutAndErrData(_stdoutAndErrData),
		  config(*_config)
	{
		#ifndef NDEBUG
			assert(_journey.failedStep != UNKNOWN_JOURNEY_STEP);
			if (defaultErrorKind == DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR) {
				assert(!responseDir.empty());
				assert(loadExceptionInfoFromResponseDir);
			}
		#endif

		if (!responseDir.empty()) {
			// May modify errorSummary, problemDescription, solutionDescription,
			// envvars, ulimits, errorKind
			loadInfoFromResponseDir(responseDir, loadExceptionInfoFromResponseDir,
				defaultErrorKind == DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR);
		}
		if (problemDescription.empty()) {
			problemDescription = createDefaultProblemDescription(_config,
				_journey, errorKind, lowLevelErrorMessage);
		}
		if (solutionDescription.empty()) {
			solutionDescription = createDefaultSolutionDescription(_config,
				_journey.failedStep, errorKind);
		}
		if (envvars.empty()) {
			envvars = gatherEnvvars();
		}
		if (ulimits.empty()) {
			ulimits = gatherUlimits();
		}
		config.internStrings();
	}

	SpawnException(const std::exception &originalException,
		const Config *_config, const Journey &_journey,
		const string &_stdoutAndErrData = string(),
		const string &responseDir = string())
		: journey(_journey),
		  errorKind(inferErrorKindFromAnotherException(originalException, _journey.failedStep)),
		  summary(createDefaultSummary(originalException, _journey)),
		  envvars(gatherEnvvars()),
		  ulimits(gatherUlimits()),
		  systemMetrics(gatherSystemMetrics()),
		  stdoutAndErrData(_stdoutAndErrData),
		  config(*_config)
	{
		assert(_journey.failedStep != UNKNOWN_JOURNEY_STEP);
		if (!responseDir.empty()) {
			loadInfoFromResponseDir(responseDir, false, false);
		}
		if (problemDescription.empty()) {
			problemDescription = createDefaultProblemDescription(_config,
				_journey, errorKind);
		}
		if (solutionDescription.empty()) {
			solutionDescription = createDefaultSolutionDescription(_config,
				_journey.failedStep, errorKind, &originalException);
		}
		config.internStrings();
	}

	virtual ~SpawnException() throw() {}

	virtual const char *what() const throw() {
		return summary.c_str();
	}

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

	const Journey &getJourney() const {
		return journey;
	}

	ErrorKind getErrorKind() const {
		return errorKind;
	}

	const string &getSystemMetrics() const {
		return systemMetrics;
	}

	const Config &getConfig() const {
		return config;
	}

	const string &getStdouterrData() const {
		return stdoutAndErrData;
	}

	string operator[](const string &name) const {
		return get(name);
	}

	string get(const HashedStaticString &name) const {
		return annotations.lookupCopy(name);
	}

	void set(const HashedStaticString &name, const string &value) {
		annotations.insert(name, value, true);
	}


	static StaticString errorKindToString(ErrorKind errorKind) {
		switch (errorKind) {
		case INTERNAL_ERROR:
			return P_STATIC_STRING("INTERNAL_ERROR");
		case OPERATING_SYSTEM_ERROR:
			return P_STATIC_STRING("OPERATING_SYSTEM_ERROR");
		case IO_ERROR:
			return P_STATIC_STRING("IO_ERROR");
		case TIMEOUT_ERROR:
			return P_STATIC_STRING("TIMEOUT_ERROR");

		case UNKNOWN_ERROR_KIND:
			return P_STATIC_STRING("UNKNOWN_ERROR_KIND");
		case DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR:
			return P_STATIC_STRING("DETERMINE_ERROR_KIND_FROM_RESPONSE_DIR");

		default:
			return P_STATIC_STRING("(invalid value)");
		}
	}
};


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_EXCEPTIONS_H_ */
