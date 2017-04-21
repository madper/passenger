#include <TestSupport.h>
#include <Core/SpawningKit/Handshake/Perform.h>

using namespace Passenger;
using namespace Passenger::SpawningKit;

namespace tut {
	struct Core_SpawningKit_HandshakePerformTest {
	};

	DEFINE_TEST_GROUP(Core_SpawningKit_HandshakePerformTest);

	/***** General logic *****/

	TEST_METHOD(1) {
		set_test_name("If the app is generic, it finishes when the app is pingable");
	}

	TEST_METHOD(2) {
		set_test_name("If findFreePort is true, it finishes when the app is pingable");
		set_test_name("It finishes when the app has sent the finish signal");
		set_test_name("It raises an error if process exits prematurely");
		set_test_name("It raises an error if the process closes stdout and stderr prematurely");
		set_test_name("It raises an error if the procedure took too long");
		set_test_name("In the event of an error, it sets the SPAWNING_KIT_HANDSHAKE_PERFORM step to the errored state");
		set_test_name("In the event of an error, the exception contains journey state information from the response directory");
		set_test_name("In the event of an error, the exception contains subprocess stdout and stderr data");
		set_test_name("In the event of an error, the exception contains messages from the subprocess as dumped in the response directory");
		set_test_name("In the event of success, it loads the journey state information from the response directory");
	}

	/***** Success response handling *****/

	TEST_METHOD(20) {
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

	TEST_METHOD(40) {
		set_test_name("It raises an error if the application responded with an error");
	}
}
