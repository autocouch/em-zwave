module EventMachine
  class Zwave
    class Node

      def initialize(home_id, node_id)
        @home_id = home_id
        @node_id = node_id
      end

      def request_info; end
      def request_dynamic; end

      def state; end
      def listening_device?; end
      def frequent_listening_device?; end
      def beaming_device?; end
      def routing_device?; end
      def security_device?; end

      def max_baud_rate; end
      def version; end
      def security; end

      def basic_type; end
      def generic_type; end
      def specific_type; end
      def type; end

      def manufacturer_name; end
      def manufacturer_id; end
      def product_name; end
      def product_type; end
      def product_id; end
      def name; end
      def location; end

      def values; end

      def on!; end
      def off!; end
      def level; end

    end
  end
end
