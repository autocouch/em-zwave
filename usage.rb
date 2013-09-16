$LOAD_PATH.unshift File.join(File.dirname(__FILE__), 'lib')

require 'eventmachine'
require 'em-zwave'

EM.run do
  @zwave = EM::Zwave.new
  @zwave.add_device('/dev/ttyUSB0')

  @zwave.on_notification do |notification|
    puts "Notification in user code: #{notification}"
  end

  @zwave.on_shutdown do
    EM.stop
  end

  Signal.trap('INT')  { @zwave.shutdown }
  Signal.trap('TERM') { @zwave.shutdown }

  EM::PeriodicTimer.new(1) { puts 'R: .' }
end
