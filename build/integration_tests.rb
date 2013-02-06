#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2010 Phusion
#
#  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
#
#  See LICENSE file for license information.

### Integration tests ###

desc "Run all integration tests"
task 'test:integration' => ['test:integration:apache2', 'test:integration:nginx'] do
end

dependencies = [:apache2, NATIVE_SUPPORT_TARGET, 'test/support/allocate_memory'].compact
desc "Run Apache 2 integration tests"
task 'test:integration:apache2' => dependencies do
	if PlatformInfo.rspec.nil?
		abort "RSpec is not installed for Ruby interpreter '#{PlatformInfo.ruby_command}'. Please install it."
	else
		Dir.chdir("test") do
			ruby "#{PlatformInfo.rspec} -c -f s integration_tests/apache2_tests.rb"
		end
	end
end

dependencies = [:nginx, NATIVE_SUPPORT_TARGET].compact
desc "Run Nginx integration tests"
task 'test:integration:nginx' => dependencies do
	if PlatformInfo.rspec.nil?
		abort "RSpec is not installed for Ruby interpreter '#{PlatformInfo.ruby_command}'. Please install it."
	else
		Dir.chdir("test") do
			ruby "#{PlatformInfo.rspec} -c -f s integration_tests/nginx_tests.rb"
		end
	end
end

dependencies = [:apache2, NATIVE_SUPPORT_TARGET].compact
desc "Run the 'restart' integration test infinitely, and abort if/when it fails"
task 'test:restart' => dependencies do
	Dir.chdir("test") do
		color_code_start = "\e[33m\e[44m\e[1m"
		color_code_end = "\e[0m"
		i = 1
		while true do
			puts "#{color_code_start}Test run #{i} (press Ctrl-C multiple times to abort)#{color_code_end}"
			sh "spec -c -f s integration_tests/apache2.rb -e 'mod_passenger running in Apache 2 : MyCook(tm) beta running on root URI should support restarting via restart.txt'"
			i += 1
		end
	end
end
