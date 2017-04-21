#include <TestSupport.h>
#include <Core/SpawningKit/Handshake/Prepare.h>
#include <unistd.h>
#include <Utils.h>

using namespace Passenger;
using namespace Passenger::SpawningKit;

namespace tut {
	struct Core_SpawningKit_HandshakePrepareTest {
		SpawningKit::Context context;
		SpawningKit::Config config;
		boost::shared_ptr<HandshakeSession> session;

		Core_SpawningKit_HandshakePrepareTest() {
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

		void initAndExec(JourneyType type, const Json::Value &extraArgs = Json::Value()) {
			vector<StaticString> errors;
			ensure("Config is valid", config.validate(errors));
			session = boost::make_shared<HandshakeSession>(context, config, type);
			session->journey.setStepInProgress(SPAWNING_KIT_PREPARATION);
			HandshakePrepare(*session, extraArgs).execute();
		}
	};

	DEFINE_TEST_GROUP(Core_SpawningKit_HandshakePrepareTest);

	TEST_METHOD(1) {
		set_test_name("It resolves the user and group ID");

		initAndExec(SPAWN_DIRECTLY);

		ensure_equals("UID is resolved", session->uid, getuid());
		ensure_equals("GID is resolved", session->gid, getgid());
		ensure("Home dir is resolved", !session->homedir.empty());
		ensure("Shell is resolved", !session->shell.empty());
	}

	TEST_METHOD(2) {
		set_test_name("It raises an error if the user does not exist");

		config.user = "doesnotexist";
		try {
			initAndExec(SPAWN_DIRECTLY);
			fail("Exception expected");
		} catch (const SpawnException &) {
			// Pass
		}
	}

	TEST_METHOD(3) {
		set_test_name("It raises an error if the group does not exist");

		config.group = "doesnotexist";
		try {
			initAndExec(SPAWN_DIRECTLY);
			fail("Exception expected");
		} catch (const SpawnException &) {
			// Pass
		}
	}

	TEST_METHOD(5) {
		set_test_name("It creates a work directory");
		fail();
	}

	TEST_METHOD(6) {
		set_test_name("It infers the application code revision from a REVISION file");
		fail();
	}

	TEST_METHOD(7) {
		set_test_name("It infers the application code revision from the"
			" Capistrano-style symlink in the app root path");
		fail();
	}

	TEST_METHOD(10) {
		set_test_name("In case of a generic app, it finds a free port for the app to listen on");
		fail();
	}

	TEST_METHOD(11) {
		set_test_name("If findFreePort is true, it finds a free port");
		fail();
	}

	TEST_METHOD(15) {
		set_test_name("It dumps arguments into the work directory");
		fail();
	}

	TEST_METHOD(16) {
		set_test_name("It adjusts the timeout when done");
		fail();
	}

	TEST_METHOD(17) {
		set_test_name("Upon throwing an exception, it sets the SPAWNING_KIT_PREPARATION step to errored");
		fail();
	}
}
