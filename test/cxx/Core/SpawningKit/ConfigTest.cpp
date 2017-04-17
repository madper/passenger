#include <TestSupport.h>
#include <Core/SpawningKit/Config.h>
#include <cstdlib>
#include <cstring>

using namespace std;
using namespace Passenger;
using namespace Passenger::SpawningKit;

namespace tut {
	struct Core_SpawningKit_ConfigTest {
		SpawningKit::Config config;

		Core_SpawningKit_ConfigTest() {
		}
	};

	DEFINE_TEST_GROUP(Core_SpawningKit_ConfigTest);

	TEST_METHOD(1) {
		set_test_name("internStrings() internalizes all strings into the object");

		char *str = (char *) malloc(32);
		strncpy(str, "hello", 32);
		config.appType = str;
		config.internStrings();

		strncpy(str, "world", 32);
		free(str);
		ensure_equals(config.appType, P_STATIC_STRING("hello"));
	}

	TEST_METHOD(2) {
		set_test_name("internStrings() works when called twice");

		config.appType = "hello";
		config.internStrings();
		config.internStrings();

		ensure_equals(config.appType, P_STATIC_STRING("hello"));
	}

	TEST_METHOD(3) {
		set_test_name("validate() works");
		vector<StaticString> errors;

		ensure("Validation fails", !config.validate(errors));
		ensure("There are errors", !errors.empty());
	}
}
