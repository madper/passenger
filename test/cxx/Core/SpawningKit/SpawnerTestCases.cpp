// Included in DirectSpawnerTest.cpp and SmartSpawnerTest.cpp.

	typedef boost::shared_ptr<Spawner> SpawnerPtr;

	TEST_METHOD(1) {
		set_test_name("Basic spawning test");
		Options options = createOptions();
		options.appRoot      = "stub/rack";
		options.startCommand = "ruby\t" "start.rb";
		options.startupFile  = "start.rb";
		SpawnerPtr spawner = createSpawner(options);
		result = spawner->spawn(options);
		ensure_equals(result["sockets"].size(), 1u);

		FileDescriptor fd(connectToServer(result["sockets"][0]["address"].asCString(),
			__FILE__, __LINE__), NULL, 0);
		writeExact(fd, "ping\n");
		ensure_equals(readAll(fd), "pong\n");
	}

	TEST_METHOD(2) {
		set_test_name("It enforces the given start timeout");
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "sleep\t" "60";
		options.startupFile  = ".";
		options.startTimeout = 100;
		setLogLevel(LVL_CRIT);

		EVENTUALLY(5,
			SpawnerPtr spawner = createSpawner(options);
			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				result = e.getErrorKind() == SpawnException::APP_STARTUP_TIMEOUT;
				if (!result) {
					// It didn't work, maybe because the server is too busy.
					// Try again with higher timeout.
					options.startTimeout = std::min<unsigned int>(
						options.startTimeout * 2, 1000);
				}
			}
		);
	}

	TEST_METHOD(3) {
		set_test_name("Any protocol errors during startup are caught and result in exceptions");
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "echo\t" "!> hello world";
		options.startupFile  = ".";
		SpawnerPtr spawner = createSpawner(options);
		setLogLevel(LVL_CRIT);
		try {
			spawner->spawn(options);
			fail("SpawnException expected");
		} catch (const SpawnException &e) {
			ensure_equals(e.getErrorKind(),
				SpawnException::APP_STARTUP_PROTOCOL_ERROR);
		}
	}

	TEST_METHOD(4) {
		set_test_name("The application may respond with a special Error response, "
			"which will result in a SpawnException with the content");
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "perl\t" "start_error.pl";
		options.startupFile  = "start_error.pl";
		SpawnerPtr spawner = createSpawner(options);
		setLogLevel(LVL_CRIT);
		try {
			spawner->spawn(options);
			fail("SpawnException expected");
		} catch (const SpawnException &e) {
			ensure_equals(e.getErrorKind(),
				SpawnException::APP_STARTUP_EXPLAINABLE_ERROR);
			ensure_equals(e.getErrorPage(),
				"He's dead, Jim!\n"
				"Relax, I'm a doctor.\n");
		}
	}

	TEST_METHOD(5) {
		set_test_name("The start timeout is enforced even while reading the error response");
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "perl\t" "start_error.pl\t" "freeze";
		options.startupFile  = "start_error.pl";
		options.startTimeout = 100;
		setLogLevel(LVL_CRIT);

		EVENTUALLY(5,
			SpawnerPtr spawner = createSpawner(options);
			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				result = e.getErrorKind() == SpawnException::APP_STARTUP_TIMEOUT;
				if (!result) {
					// It didn't work, maybe because the server is too busy.
					// Try again with higher timeout.
					options.startTimeout = std::min<unsigned int>(
						options.startTimeout * 2, 1000);
				}
			}
		);
	}

	TEST_METHOD(6) {
		set_test_name("The reported PID is correct");
		Options options = createOptions();
		options.appRoot      = "stub/rack";
		options.startCommand = "ruby\t" "start.rb";
		options.startupFile  = "start.rb";
		SpawnerPtr spawner = createSpawner(options);
		result = spawner->spawn(options);
		ensure_equals(result["sockets"].size(), 1u);

		FileDescriptor fd(connectToServer(result["sockets"][0]["address"].asCString(),
			__FILE__, __LINE__), NULL, 0);
		writeExact(fd, "pid\n");
		ensure_equals(readAll(fd), toString(result["pid"].asInt()) + "\n");
	}

	TEST_METHOD(7) {
		set_test_name("Custom environment variables can be passed");
		string envvars = modp::b64_encode("PASSENGER_FOO\0foo\0PASSENGER_BAR\0bar\0",
			sizeof("PASSENGER_FOO\0foo\0PASSENGER_BAR\0bar\0") - 1);
		Options options = createOptions();
		options.appRoot = "stub/rack";
		options.startCommand = "ruby\t" "start.rb";
		options.startupFile  = "start.rb";
		options.environmentVariables = envvars;
		SpawnerPtr spawner = createSpawner(options);
		result = spawner->spawn(options);
		ensure_equals(result["sockets"].size(), 1u);

		FileDescriptor fd(connectToServer(result["sockets"][0]["address"].asCString(),
			__FILE__, __LINE__), NULL, 0);
		writeExact(fd, "envvars\n");
		envvars = readAll(fd);
		ensure("(1)", envvars.find("PASSENGER_FOO = foo\n") != string::npos);
		ensure("(2)", envvars.find("PASSENGER_BAR = bar\n") != string::npos);
	}

	TEST_METHOD(8) {
		set_test_name("Any raised SpawnExceptions take note of the process's environment variables");
		string envvars = modp::b64_encode("PASSENGER_FOO\0foo\0",
			sizeof("PASSENGER_FOO\0foo\0") - 1);
		Options options = createOptions();
		options.appRoot      = "stub";
		options.startCommand = "echo\t" "!> hello world";
		options.startupFile  = ".";
		options.environmentVariables = envvars;
		SpawnerPtr spawner = createSpawner(options);
		setLogLevel(LVL_CRIT);
		try {
			spawner->spawn(options);
			fail("SpawnException expected");
		} catch (const SpawnException &e) {
			ensure(containsSubstring(e["envvars"], "PASSENGER_FOO=foo\n"));
		}
	}

	TEST_METHOD(9) {
		set_test_name("It raises an exception if the user does not have a access to one "
			"of the app root's parent directories, or the app root itself");
		runShellCommand("mkdir -p tmp.check/a/b/c");
		TempDirCopy dir("stub/rack", "tmp.check/a/b/c/d");
		TempDir dir2("tmp.check");

		char buffer[PATH_MAX];
		string cwd = getcwd(buffer, sizeof(buffer));

		Options options = createOptions();
		options.appRoot = "tmp.check/a/b/c/d";
		options.appType = "rack";
		SpawnerPtr spawner = createSpawner(options);
		setLogLevel(LVL_CRIT);

		if (getuid() != 0) {
			// TODO: implement this test for root too
			runShellCommand("chmod 000 tmp.check/a/b/c/d");
			runShellCommand("chmod 600 tmp.check/a/b/c");
			runShellCommand("chmod 600 tmp.check/a");

			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				ensure("(1)", containsSubstring(e.getErrorPage(),
					"the parent directory '" + cwd + "/tmp.check/a' has wrong permissions"));
			}

			runShellCommand("chmod 700 tmp.check/a");
			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				ensure("(2)", containsSubstring(e.getErrorPage(),
					"the parent directory '" + cwd + "/tmp.check/a/b/c' has wrong permissions"));
			}

			runShellCommand("chmod 700 tmp.check/a/b/c");
			try {
				spawner->spawn(options);
				fail("SpawnException expected");
			} catch (const SpawnException &e) {
				ensure("(3)", containsSubstring(e.getErrorPage(),
					"However this directory is not accessible because it has wrong permissions."));
			}

			runShellCommand("chmod 700 tmp.check/a/b/c/d");
			spawner->spawn(options); // Should not throw.
		}
	}

	TEST_METHOD(11) {
		set_test_name("It infers the code revision from the REVISION file");
		TempDirCopy dir("stub/rack", "tmp.rack");
		createFile("tmp.rack/REVISION", "hello\n");

		Options options = createOptions();
		options.appRoot      = "tmp.rack";
		options.startCommand = "ruby\t" "start.rb";
		options.startupFile  = "start.rb";
		SpawnerPtr spawner = createSpawner(options);
		result = spawner->spawn(options);

		ensure_equals(result["code_revision"].asString(), "hello");
	}

	TEST_METHOD(12) {
		set_test_name("It infers the code revision from the app root symlink, "
			"if the app root is called 'current'");
		TempDir dir1("tmp.rack");
		TempDirCopy dir2("stub/rack", "tmp.rack/today");
		symlink("today", "tmp.rack/current");

		Options options = createOptions();
		options.appRoot      = "tmp.rack/current";
		options.startCommand = "ruby\t" "start.rb";
		options.startupFile  = "start.rb";
		SpawnerPtr spawner = createSpawner(options);
		result = spawner->spawn(options);

		ensure_equals(result["code_revision"].asString(), "today");
	}
