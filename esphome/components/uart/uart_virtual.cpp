#include "esphome/core/defines.h"
#ifdef USE_UART_VIRTUAL

#include "uart_virtual.h"

namespace esphome::uart {

  }
}

bool VirtualUARTComponent::inject_rx(const uint8_t *data, size_t len) {
  }
  }
  return true;
}

bool VirtualUARTComponent::peek_byte(uint8_t *data) {
    return false;
  }
  return true;
}

bool VirtualUARTComponent::read_array(uint8_t *data, size_t len) {
    return false;
  }
#ifdef USE_UART_DEBUGGER
#endif
  return true;
}

}  // namespace esphome::uart

#endif  // USE_UART_VIRTUAL
