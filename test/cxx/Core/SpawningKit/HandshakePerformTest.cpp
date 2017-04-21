#include <TestSupport.h>
#include <Core/SpawningKit/Handshake/Prepare.h>
#include <Core/SpawningKit/Handshake/Perform.h>
#include <boost/bind.hpp>
#include <cstdio>
#include <Utils/IOUtils.h>

using namespace std;
using namespace Passenger;
using namespace Passenger::SpawningKit;

namespace tut {
	struct Core_SpawningKit_HandshakePerformTest {
		SpawningKit::Context::Schema schema;
		SpawningKit::Context context;
		SpawningKit::Config config;
		boost::shared_ptr<HandshakeSession> session;
		pid_t pid;
		HandshakePerform::DebugSupport *debugSupport;
		AtomicInt counter;
		FileDescriptor server;

		Core_SpawningKit_HandshakePerformTest()
			: context(schema),
			  pid(getpid()),
			  debugSupport(NULL)
		{
			context.resourceLocator = resourceLocator;
			context.integrationMode = "standalone";
			context.finalize();

			config.appRoot = "/tmp/myapp";
			config.startCommand = "echo hi";
			config.startupFile = "/tmp/myapp/app.py";
			config.appType = "wsgi";
			config.spawnMethod = "direct";
			config.user = getProcessUsername();
			config.group = getGroupName(getgid());
			config.internStrings();
		}

		void init(JourneyType type) {
			vector<StaticString> errors;
			ensure("Config is valid", config.validate(errors));
			session = boost::make_shared<HandshakeSession>(context, config, type);

			session->journey.setStepInProgress(SPAWNING_KIT_PREPARATION);
			HandshakePrepare(*session).execute();

			session->journey.setStepInProgress(SPAWNING_KIT_HANDSHAKE_PERFORM);
			session->journey.setStepInProgress(SUBPROCESS_BEFORE_FIRST_EXEC);
		}

		void execute() {
			HandshakePerform performer(*session, pid);
			performer.debugSupport = debugSupport;
			performer.execute();
			counter++;
		}
	};

	DEFINE_TEST_GROUP_WITH_LIMIT(Core_SpawningKit_HandshakePerformTest, 80);


	/***** General logic *****/

	struct Test1DebugSupport: public HandshakePerform::DebugSupport {
		HandshakeSession *session;
		AtomicInt expectedStartPort;

		virtual void beginWaitUntilSpawningFinished() {
			expectedStartPort = session->expectedStartPort;
		}
	};

	TEST_METHOD(1) {
		set_test_name("If the app is generic, it finishes when the app is pingable");

		Test1DebugSupport debugSupport;
		this->debugSupport = &debugSupport;
		config.genericApp = true;
		init(SPAWN_DIRECTLY);
		debugSupport.session = session.get();
		TempThread thr(boost::bind(&Core_SpawningKit_HandshakePerformTest::execute, this));

		SHOULD_NEVER_HAPPEN(100,
			result = counter > 0;
		);

		ensure(debugSupport.expectedStartPort.get() != 0);
		server.assign(createTcpServer("127.0.0.1", debugSupport.expectedStartPort.get()),
			NULL, 0);

		EVENTUALLY(1,
			result = counter == 1;
		);
	}

	TEST_METHOD(2) {
		set_test_name("If findFreePort is true, it finishes when the app is pingable");

		Test1DebugSupport debugSupport;
		this->debugSupport = &debugSupport;
		config.findFreePort = true;
		init(SPAWN_DIRECTLY);
		debugSupport.session = session.get();
		TempThread thr(boost::bind(&Core_SpawningKit_HandshakePerformTest::execute, this));

		SHOULD_NEVER_HAPPEN(100,
			result = counter > 0;
		);

		ensure(debugSupport.expectedStartPort.get() != 0);
		server.assign(createTcpServer("127.0.0.1", debugSupport.expectedStartPort.get()),
			NULL, 0);

		EVENTUALLY(1,
			result = counter == 1;
		);
	}

	TEST_METHOD(3) {
		set_test_name("It finishes when the app has sent the finish signal");

		init(SPAWN_DIRECTLY);
		TempThread thr(boost::bind(&Core_SpawningKit_HandshakePerformTest::execute, this));

		SHOULD_NEVER_HAPPEN(100,
			result = counter > 0;
		);

		Json::Value socket, doc;
		socket["address"] = "tcp://127.0.0.1:3000";
		socket["protocol"] = "http";
		socket["concurrency"] = 1;
		socket["accept_http_requests"] = true;
		doc["sockets"].append(socket);
		createFile(session->responseDir + "/properties.json", doc.toStyledString());

		FILE *f = fopen((session->responseDir + "/finish").c_str(), "w");
		fwrite("1", 1, 1, f);
		fclose(f);

		EVENTUALLY(1,
			result = counter == 1;
		);
	}

	TEST_METHOD(10) {
		set_test_name("It raises an error if the process exits prematurely");

		init(SPAWN_DIRECTLY);
		pid = fork();
		if (pid == 0) {
			// Exit child
			_exit(1);
		}

		try {
			execute();
			fail("SpawnException expected");
		} catch (const SpawnException &e) {
			ensure_equals(StaticString(e.what()),
				"The application process exited prematurely.");
		}
	}

	TEST_METHOD(11) {
		set_test_name("It raises an error if the procedure took too long");

		config.startTimeoutMsec = 50;
		init(SPAWN_DIRECTLY);
		pid = fork();
		if (pid == 0) {
			// Exit child
			usleep(1000000);
			_exit(1);
		}

		try {
			execute();
			fail("SpawnException expected");
		} catch (const SpawnException &e) {
			ensure_equals(StaticString(e.what()),
				"A timeout error occurred while spawning an application process.");
		}
	}

	struct Test15DebugSupport: public HandshakePerform::DebugSupport {
		virtual void beginWaitUntilSpawningFinished() {
			throw RuntimeException("oh no!");
		}
	};

	TEST_METHOD(15) {
		set_test_name("In the event of an error, it sets the SPAWNING_KIT_HANDSHAKE_PERFORM step to the errored state");

		Test15DebugSupport debugSupport;
		this->debugSupport = &debugSupport;
		init(SPAWN_DIRECTLY);

		try {
			execute();
			fail("SpawnException expected");
		} catch (const SpawnException &) {
			ensure_equals(session->journey.getFirstFailedStep(), SPAWNING_KIT_HANDSHAKE_PERFORM);
		}
	}

	TEST_METHOD(16) {
		set_test_name("In the event of an error, the exception contains journey state information from the response directory");

		Test15DebugSupport debugSupport;
		this->debugSupport = &debugSupport;
		init(SPAWN_DIRECTLY);

		createFile(session->responseDir + "/steps/subprocess_listen/state", "STEP_ERRORED");

		try {
			execute();
			fail("SpawnException expected");
		} catch (const SpawnException &) {
			ensure_equals(session->journey.getStepInfo(SUBPROCESS_LISTEN).state,
				STEP_ERRORED);
		}
	}

	TEST_METHOD(17) {
		set_test_name("In the event of an error, the exception contains subprocess stdout and stderr data");
		fail();
	}

	TEST_METHOD(18) {
		set_test_name("In the event of an error, the exception contains messages from the subprocess as dumped in the response directory");
		fail();
	}

	TEST_METHOD(19) {
		set_test_name("In the event of success, it loads the journey state information from the response directory");
		fail();
	}


	/***** Success response handling *****/

	TEST_METHOD(30) {
		set_test_name("The result object contains basic information such as FDs and time");
		set_test_name("The result object contains sockets specified in properties.json");
		set_test_name("If the app is generic, it automatically registers the free port as a request-handling socket");
		set_test_name("If findFreePort is true, it automatically registers the free port as a request-handling socket");
		set_test_name("It raises an error if we expected the subprocess to create a properties.json,"
			" but the file does not conform to the required format");
		set_test_name("It raises an error if we expected the subprocess to specify at"
			" least one request-handling socket in properties.json, yet the file does"
			" not specify any");
		set_test_name("It raises an error if we expected the subprocess to specify at"
			" least one request-handling socket in properties.json, yet properties.json"
			" does not exist");
		set_test_name("It raises an error if we expected the subprocess to specify at"
			" least one preloader command socket in properties.json, yet the file does"
			" not specify any");
		set_test_name("It raises an error if we expected the subprocess to specify at"
			" least one preloader command socket in properties.json, yet properties.json"
			" does not exist");
	}


	/***** Error response handling *****/

	TEST_METHOD(50) {
		set_test_name("It raises an error if the application responded with an error");
		fail();
	}
}
