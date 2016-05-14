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
#ifndef _PASSENGER_SPAWNING_KIT_DIRECT_SPAWNER_H_
#define _PASSENGER_SPAWNING_KIT_DIRECT_SPAWNER_H_

#include <Core/SpawningKit/Spawner.h>
#include <Core/SpawningKit/HandshakePrepare.h>
#include <Core/SpawningKit/HandshakePerform.h>
#include <Constants.h>
#include <Logging.h>
#include <Utils/IOUtils.h>

#include <LveLoggingDecorator.h>
#include <limits.h>  // for PTHREAD_STACK_MIN
#include <pthread.h>
#include <adhoc_lve.h>

namespace Passenger {
namespace SpawningKit {

using namespace std;
using namespace boost;
using namespace oxt;


class DirectSpawner: public Spawner {
private:
	static int startBackgroundThread(void *(*mainFunction)(void *), void *arg) {
		// Using raw pthread API because we don't want to register such
		// trivial threads on the oxt::thread list.
		pthread_t thr;
		pthread_attr_t attr;
		size_t stack_size = 96 * 1024;

		unsigned long min_stack_size;
		bool stack_min_size_defined;
		bool round_stack_size;
		int ret;

		#ifdef PTHREAD_STACK_MIN
			// PTHREAD_STACK_MIN may not be a constant macro so we need
			// to evaluate it dynamically.
			min_stack_size = PTHREAD_STACK_MIN;
			stack_min_size_defined = true;
		#else
			// Assume minimum stack size is 128 KB.
			min_stack_size = 128 * 1024;
			stack_min_size_defined = false;
		#endif
		if (stack_size != 0 && stack_size < min_stack_size) {
			stack_size = min_stack_size;
			round_stack_size = !stack_min_size_defined;
		} else {
			round_stack_size = true;
		}

		if (round_stack_size) {
			// Round stack size up to page boundary.
			long page_size;
			#if defined(_SC_PAGESIZE)
				page_size = sysconf(_SC_PAGESIZE);
			#elif defined(_SC_PAGE_SIZE)
				page_size = sysconf(_SC_PAGE_SIZE);
			#elif defined(PAGESIZE)
				page_size = sysconf(PAGESIZE);
			#elif defined(PAGE_SIZE)
				page_size = sysconf(PAGE_SIZE);
			#else
				page_size = getpagesize();
			#endif
			if (stack_size % page_size != 0) {
				stack_size = stack_size - (stack_size % page_size) + page_size;
			}
		}

		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, 1);
		pthread_attr_setstacksize(&attr, stack_size);
		ret = pthread_create(&thr, &attr, mainFunction, arg);
		pthread_attr_destroy(&attr);
		return ret;
	}

	static void *detachProcessMain(void *arg) {
		boost::this_thread::disable_syscall_interruption dsi;
		pid_t pid = (pid_t) (long) arg;
		syscalls::waitpid(pid, NULL, 0);
		return NULL;
	}

	void detachProcess(pid_t pid) {
		startBackgroundThread(detachProcessMain, (void *) (long) pid);
	}

	void setConfigFromAppPoolOptions(Config *config, Json::Value &extraArgs,
		const AppPoolOptions &options)
	{
		Spawner::setConfigFromAppPoolOptions(config, extraArgs, options);
		config->spawnMethod = P_STATIC_STRING("direct");
	}

public:
	DirectSpawner(const Context *context)
		: Spawner(context)
		{ }

	virtual Result spawn(const AppPoolOptions &options) {
		TRACE_POINT();
		boost::this_thread::disable_interruption di;
		boost::this_thread::disable_syscall_interruption dsi;
		P_DEBUG("Spawning new process: appRoot=" << options.appRoot);
		possiblyRaiseInternalError(options);

		Config config(options);
		Json::Value extraArgs;
		Result result;
		HandshakeSession session;
		session.context = context;
		session.config = &config;

		setConfigFromAppPoolOptions(config, extraArgs, options);
		HandshakePrepare(session, extraArgs).execute();

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
				??????, strerror(e), e);
			fflush(stderr);
			_exit(1);

		} else if (pid == -1) {
			int e = errno;
			throw SystemException("Cannot fork a new process", e);

		} else {
			UPDATE_TRACE_POINT();
			scopedLveEnter.exit();

			P_LOG_FILE_DESCRIPTOR_PURPOSE(stdinChannel.second,
				"App " << pid << " (" << options.appRoot << ") stdin");
			P_LOG_FILE_DESCRIPTOR_PURPOSE(stdoutAndErrChannel.first,
				"App " << pid << " (" << options.appRoot << ") stdoutAndErr");

			UPDATE_TRACE_POINT();
			ScopeGuard guard(boost::bind(nonInterruptableKillAndWaitpid, pid));
			P_DEBUG("Process forked for appRoot=" << options.appRoot << ": PID " << pid);
			stdinChannel.first.close();
			stdoutAndErrChannel.second.close();

			HandshakePerform(session, pid, stdinChannel.second,
				stdoutAndErrChannel.first).execute();

			UPDATE_TRACE_POINT();
			detachProcess(result["pid"].asInt());
			guard.clear();
			P_DEBUG("Process spawning done: appRoot=" << options.appRoot <<
				", pid=" << result.pid);
			return result;
		}
	}
};


} // namespace SpawningKit
} // namespace Passenger

#endif /* _PASSENGER_SPAWNING_KIT_DIRECT_SPAWNER_H_ */
