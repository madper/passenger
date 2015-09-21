#  Phusion Passenger - https://www.phusionpassenger.com/
#  Copyright (c) 2010-2014 Phusion
#
#  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
#
#  See LICENSE file for license information.

require 'optparse'
PhusionPassenger.require_passenger_lib 'constants'
PhusionPassenger.require_passenger_lib 'standalone/command'
PhusionPassenger.require_passenger_lib 'standalone/config_utils'
PhusionPassenger.require_passenger_lib 'standalone/control_utils'
PhusionPassenger.require_passenger_lib 'ruby_core_enhancements'

module PhusionPassenger
  module Standalone

    class StopCommand < Command
      def run
        @options = { :port => 3000 }
        parse_options
        find_pid_file
        create_controller
        begin
          running = @controller.running?
        rescue SystemCallError, IOError
          running = false
        end
        if running
          @controller.stop
        else
          Standalone::ControlUtils.warn_pid_file_not_found(@options)
          exit 1
        end
      end

    private
      def self.create_option_parser(options)
        OptionParser.new do |opts|
          nl = "\n" + ' ' * 37
          opts.banner = "Usage: passenger stop [OPTIONS]\n"
          opts.separator "Stops a running #{PROGRAM_NAME} Standalone instance."
          opts.separator ""

          opts.separator "Options:"
          opts.on("-p", "--port NUMBER", Integer,
            "The port number of the #{PROGRAM_NAME}#{nl}" +
            "instance. Default: 3000") do |value|
            options[:port] = value
          end
          opts.on("--pid-file FILE", String,
            "PID file of the running #{PROGRAM_NAME}#{nl}" +
            "Standalone instance") do |value|
            options[:pid_file] = value
          end
          opts.on("--ignore-pid-not-found", "-i",
            "Don't abort with an error if PID file cannot be found") do
            options[:ignore_pid_not_found] = true
          end
        end
      end

      def find_pid_file
        return if @options[:pid_file]

        ["tmp/pids", "."].each do |dir|
          path = File.absolute_path_no_resolve("#{dir}/passenger.#{@options[:port]}.pid")
          if File.exist?(path)
            @options[:pid_file] = path
            return
          end
        end

        if @options[:ignore_pid_not_found]
          exit
        else
          Standalone::ControlUtils.warn_pid_file_not_found(@options)
          exit 1
        end
      end

      def create_controller
        Standalone::ControlUtils.require_daemon_controller
        @controller = DaemonController.new(
          :identifier    => "#{PROGRAM_NAME} Standalone engine",
          :start_command => "true", # Doesn't matter
          :ping_command  => "true", # Doesn't matter
          :pid_file      => @options[:pid_file],
          :log_file      => "/dev/null",
          :timeout       => 25
        )
      end
    end

  end
end