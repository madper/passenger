/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2017 Phusion Holding B.V.
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
#ifndef _PASSENGER_SPAWNING_KIT_HANDSHAKE_JOURNEY_H_
#define _PASSENGER_SPAWNING_KIT_HANDSHAKE_JOURNEY_H_

#include <map>
#include <utility>

#include <Logging.h>
#include <StaticString.h>
#include <Utils/StrIntUtils.h>

namespace Passenger {
namespace SpawningKit {

using namespace std;


enum JourneyType {
	SPAWN_DIRECTLY,
	START_PRELOADER,
	SPAWN_THROUGH_PRELOADER
};

enum JourneyStep {
	// Steps in Passenger Core / SpawningKit
	SPAWNING_KIT_PREPARATION,
	SPAWNING_KIT_FORK_SUBPROCESS,
	SPAWNING_KIT_CONNECT_TO_PRELOADER,
	SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER,
	SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER,
	SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER,
	SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER,
	SPAWNING_KIT_HANDSHAKE_PERFORM,
	SPAWNING_KIT_FINISH,

	// Steps in preloader (when spawning a worker process)
	PRELOADER_PREPARATION,
	PRELOADER_FORK_SUBPROCESS,
	PRELOADER_SEND_RESPONSE,
	PRELOADER_FINISH,

	// Steps in subprocess
	SUBPROCESS_BEFORE_FIRST_EXEC,
	SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL,
	SUBPROCESS_OS_SHELL,
	SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL,
	SUBPROCESS_EXEC_WRAPPER,
	SUBPROCESS_WRAPPER_PREPARATION,
	SUBPROCESS_APP_LOAD_OR_EXEC,
	SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER,
	SUBPROCESS_LISTEN,
	SUBPROCESS_FINISH,

	// Other
	UNKNOWN_JOURNEY_STEP
};

enum JourneyStepState {
	/**
	 * This step has not started yet. Will be visualized with an empty
	 * placeholder.
	 */
	STEP_NOT_STARTED,

	/**
	 * This step is currently in progress. Will be visualized with a spinner.
	 */
	STEP_IN_PROGRESS,

	/**
	 * This step has already been performed successfully. Will be
	 * visualized with a green tick.
	 */
	STEP_PERFORMED,

	/**
	 * This step has failed. Will be visualized with a red mark.
	 */
	STEP_ERRORED
};

inline StaticString journeyStepToString(JourneyStep step);
inline StaticString journeyStepStateToString(JourneyStepState state);
inline JourneyStep stringToPreloaderJourneyStep(const StaticString &name);
inline JourneyStep stringToSubprocessJourneyStep(const StaticString &name);

inline JourneyStep
getFirstSubprocessJourneyStep() {
	return SUBPROCESS_BEFORE_FIRST_EXEC;
}

inline JourneyStep
getLastSubprocessJourneyStep() {
	return SUBPROCESS_FINISH;
}


class Journey {
public:
	typedef map<JourneyStep, JourneyStepState> Map;

private:
	JourneyType type;
	bool usingWrapper;
	Map steps;

	void fillInStepsForSpawnDirectlyJourney() {
		steps.insert(make_pair(SPAWNING_KIT_PREPARATION, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_FORK_SUBPROCESS, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_HANDSHAKE_PERFORM, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_FINISH, STEP_NOT_STARTED));

		steps.insert(make_pair(SUBPROCESS_BEFORE_FIRST_EXEC, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_OS_SHELL, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL, STEP_NOT_STARTED));
		if (usingWrapper) {
			steps.insert(make_pair(SUBPROCESS_EXEC_WRAPPER, STEP_NOT_STARTED));
			steps.insert(make_pair(SUBPROCESS_WRAPPER_PREPARATION, STEP_NOT_STARTED));
		}
		steps.insert(make_pair(SUBPROCESS_APP_LOAD_OR_EXEC, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_LISTEN, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_FINISH, STEP_NOT_STARTED));
	}

	void fillInStepsForPreloaderStartJourney() {
		steps.insert(make_pair(SPAWNING_KIT_PREPARATION, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_FORK_SUBPROCESS, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_HANDSHAKE_PERFORM, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_FINISH, STEP_NOT_STARTED));

		steps.insert(make_pair(SUBPROCESS_BEFORE_FIRST_EXEC, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_OS_SHELL, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL, STEP_NOT_STARTED));
		if (usingWrapper) {
			steps.insert(make_pair(SUBPROCESS_EXEC_WRAPPER, STEP_NOT_STARTED));
			steps.insert(make_pair(SUBPROCESS_WRAPPER_PREPARATION, STEP_NOT_STARTED));
		}
		steps.insert(make_pair(SUBPROCESS_APP_LOAD_OR_EXEC, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_LISTEN, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_FINISH, STEP_NOT_STARTED));
	}

	void fillInStepsForSpawnThroughPreloaderJourney() {
		steps.insert(make_pair(SPAWNING_KIT_PREPARATION, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_FORK_SUBPROCESS, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_CONNECT_TO_PRELOADER, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_HANDSHAKE_PERFORM, STEP_NOT_STARTED));
		steps.insert(make_pair(SPAWNING_KIT_FINISH, STEP_NOT_STARTED));

		steps.insert(make_pair(PRELOADER_PREPARATION, STEP_NOT_STARTED));
		steps.insert(make_pair(PRELOADER_FORK_SUBPROCESS, STEP_NOT_STARTED));
		steps.insert(make_pair(PRELOADER_SEND_RESPONSE, STEP_NOT_STARTED));
		steps.insert(make_pair(PRELOADER_FINISH, STEP_NOT_STARTED));

		steps.insert(make_pair(SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_LISTEN, STEP_NOT_STARTED));
		steps.insert(make_pair(SUBPROCESS_FINISH, STEP_NOT_STARTED));
	}

public:
	Journey(JourneyType _type, bool _usingWrapper)
		: type(_type),
		  usingWrapper(_usingWrapper)
	{
		switch (_type) {
		case SPAWN_DIRECTLY:
			fillInStepsForSpawnDirectlyJourney();
			break;
		case START_PRELOADER:
			fillInStepsForPreloaderStartJourney();
			break;
		case SPAWN_THROUGH_PRELOADER:
			fillInStepsForSpawnThroughPreloaderJourney();
			break;
		default:
			P_BUG("Unknown journey type " << toString((int) _type));
			break;
		}
	}

	JourneyType getType() const {
		return type;
	}

	const Map &getSteps() const {
		return steps;
	}

	void setStepState(JourneyStep step, JourneyStepState state) {
		Map::iterator it = steps.find(step);
		if (it == steps.end()) {
			throw RuntimeException("Invalid step " + journeyStepToString(step));
		}

		switch (it->second) {
		case STEP_NOT_STARTED:
			it->second = state;
			break;
		case STEP_IN_PROGRESS:
			if (state == STEP_PERFORMED || state == STEP_ERRORED) {
				it->second = state;
			} else {
				throw RuntimeException("Unable to change state for in-progress journey step "
					+ journeyStepToString(step) + " to " + journeyStepStateToString(state));
			}
			break;
		case STEP_PERFORMED:
		case STEP_ERRORED:
			throw RuntimeException("Unable to change state for completed journey step "
				+ journeyStepToString(step));
		default:
			P_BUG("Unknown journey step state " << toString((int) it->second));
			break;
		}
	}
};


inline StaticString
journeyStepToString(JourneyStep step) {
	switch (step) {
	case SPAWNING_KIT_PREPARATION:
		return P_STATIC_STRING("SPAWNING_KIT_PREPARATION");
	case SPAWNING_KIT_FORK_SUBPROCESS:
		return P_STATIC_STRING("SPAWNING_KIT_FORK_SUBPROCESS");
	case SPAWNING_KIT_CONNECT_TO_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_CONNECT_TO_PRELOADER");
	case SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_SEND_COMMAND_TO_PRELOADER");
	case SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_READ_RESPONSE_FROM_PRELOADER");
	case SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_PARSE_RESPONSE_FROM_PRELOADER");
	case SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER:
		return P_STATIC_STRING("SPAWNING_KIT_PROCESS_RESPONSE_FROM_PRELOADER");
	case SPAWNING_KIT_HANDSHAKE_PERFORM:
		return P_STATIC_STRING("SPAWNING_KIT_HANDSHAKE_PERFORM");
	case SPAWNING_KIT_FINISH:
		return P_STATIC_STRING("SPAWNING_KIT_FINISH");

	case PRELOADER_PREPARATION:
		return P_STATIC_STRING("PRELOADER_PREPARATION");
	case PRELOADER_FORK_SUBPROCESS:
		return P_STATIC_STRING("PRELOADER_FORK_SUBPROCESS");
	case PRELOADER_SEND_RESPONSE:
		return P_STATIC_STRING("PRELOADER_SEND_RESPONSE");
	case PRELOADER_FINISH:
		return P_STATIC_STRING("PRELOADER_FINISH");

	case SUBPROCESS_BEFORE_FIRST_EXEC:
		return P_STATIC_STRING("SUBPROCESS_BEFORE_FIRST_EXEC");
	case SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL:
		return P_STATIC_STRING("SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL");
	case SUBPROCESS_OS_SHELL:
		return P_STATIC_STRING("SUBPROCESS_OS_SHELL");
	case SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL:
		return P_STATIC_STRING("SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL");
	case SUBPROCESS_WRAPPER_PREPARATION:
		return P_STATIC_STRING("SUBPROCESS_WRAPPER_PREPARATION");
	case SUBPROCESS_APP_LOAD_OR_EXEC:
		return P_STATIC_STRING("SUBPROCESS_APP_LOAD_OR_EXEC");
	case SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER:
		return P_STATIC_STRING("SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER");
	case SUBPROCESS_LISTEN:
		return P_STATIC_STRING("SUBPROCESS_LISTEN");
	case SUBPROCESS_FINISH:
		return P_STATIC_STRING("SUBPROCESS_FINISH");

	default:
		return P_STATIC_STRING("UNKNOWN_JOURNEY_STEP");
	}
}

inline StaticString
journeyStepStateToString(JourneyStepState state) {
	switch (state) {
	case STEP_NOT_STARTED:
		return P_STATIC_STRING("STEP_NOT_STARTED");
	case STEP_IN_PROGRESS:
		return P_STATIC_STRING("STEP_IN_PROGRESS");
	case STEP_PERFORMED:
		return P_STATIC_STRING("STEP_PERFORMED");
	case STEP_ERRORED:
		return P_STATIC_STRING("STEP_ERRORED");
	default:
		return P_STATIC_STRING("UNKNOWN_JOURNEY_STEP_STATE");
	}
}

inline JourneyStep
stringToPreloaderJourneyStep(const StaticString &name) {
	if (name == P_STATIC_STRING("PRELOADER_PREPARATION")) {
		return PRELOADER_PREPARATION;
	} else if (name == P_STATIC_STRING("PRELOADER_FORK_SUBPROCESS")) {
		return PRELOADER_FORK_SUBPROCESS;
	} else if (name == P_STATIC_STRING("PRELOADER_SEND_RESPONSE")) {
		return PRELOADER_SEND_RESPONSE;
	} else if (name == P_STATIC_STRING("PRELOADER_FINISH")) {
		return PRELOADER_FINISH;
	} else {
		return UNKNOWN_JOURNEY_STEP;
	}
}

inline JourneyStep
stringToSubprocessJourneyStep(const StaticString &name) {
	if (name == P_STATIC_STRING("SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL")) {
		return SUBPROCESS_SPAWN_ENV_SETUPPER_BEFORE_SHELL;
	} else if (name == P_STATIC_STRING("SUBPROCESS_OS_SHELL")) {
		return SUBPROCESS_OS_SHELL;
	} else if (name == P_STATIC_STRING("SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL")) {
		return SUBPROCESS_SPAWN_ENV_SETUPPER_AFTER_SHELL;
	} else if (name == P_STATIC_STRING("SUBPROCESS_EXEC_WRAPPER")) {
		return SUBPROCESS_EXEC_WRAPPER;
	} else if (name == P_STATIC_STRING("SUBPROCESS_WRAPPER_PREPARATION")) {
		return SUBPROCESS_WRAPPER_PREPARATION;
	} else if (name == P_STATIC_STRING("SUBPROCESS_APP_LOAD_OR_EXEC")) {
		return SUBPROCESS_APP_LOAD_OR_EXEC;
	} else if (name == P_STATIC_STRING("SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER")) {
		return SUBPROCESS_PREPARE_AFTER_FORKING_FROM_PRELOADER;
	} else if (name == P_STATIC_STRING("SUBPROCESS_LISTEN")) {
		return SUBPROCESS_LISTEN;
	} else if (name == P_STATIC_STRING("SUBPROCESS_FINISH")) {
		return SUBPROCESS_FINISH;
	} else {
		return UNKNOWN_JOURNEY_STEP;
	}
}


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_HANDSHAKE_JOURNEY_H_ */
