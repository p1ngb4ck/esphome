"""Per-port FIFO bias for the espressif/usb host component.

The DWC controller splits one FIFO between received data (RX), non-periodic OUT and
periodic OUT, and that split is what caps the largest packet each direction can carry.
_calculate_fifo_from_bias() in hcd_dwc.c already derives the split per port from that
port's own OTG_DFIFO_DEPTH, but which of the three biases it applies comes from a single
Kconfig choice that reaches every port at once.

On a chip with two host controllers the two ports want opposite things. A full-speed port
carrying an isochronous OUT stream needs the periodic-out split, and the same split on a
high-speed port leaves 256 bytes of non-periodic OUT, below the 512 byte wMaxPacketSize
that every high-speed bulk OUT endpoint has by definition, so hcd_pipe_alloc() rejects all
of them. One choice cannot serve both.

The plumbing for a per-port answer is already there: hcd_port_config_t carries a per-port
fifo_config and hcd_port_init() validates it against that port's own HAL. Only the choice
is global. These patches add a per-port bias that travels the same path as the existing
per-port fifo_config -- usb_host_config_t, hub_config_t, hcd_port_config_t -- and turn the
preprocessor chain in _calculate_fifo_from_bias() into a switch over it. A port left at
USB_HOST_FIFO_BIAS_DEFAULT resolves to the Kconfig choice, so a caller that sets nothing
sees no change.

Deliberately expressed as exact-string replacements rather than regexes: an upstream
change to any of these lines has to surface as a build-time failure naming the file, not
as a patch that silently applies to something else.
"""

from __future__ import annotations

import hashlib

# Version of the espressif/usb component these anchors were taken from. The download URL
# and the patches move together; bumping one without the other is a build failure.
USB_COMPONENT_VERSION = "1.4.1"
USB_COMPONENT_URL = (
    "https://components-file.espressif.com/components/espressif/usb/"
    f"{USB_COMPONENT_VERSION}/espressif__usb-v{USB_COMPONENT_VERSION}.zip"
)

# Bias values, mirrored in usb_host.h on the ESPHome side and in the enum below.
BIAS_DEFAULT = 0
BIAS_BALANCED = 1
BIAS_IN = 2
BIAS_PERIODIC_OUT = 3


_PUBLIC_HEADER_ANCHOR = """typedef struct {
    bool skip_phy_setup;                        /**< If set, the USB Host Library will not configure the USB PHY thus allowing"""

_PUBLIC_HEADER_ADDITION = """/**
 * @brief Hardware FIFO size biasing, per root port
 *
 * The DWC controller divides one FIFO between received data, non-periodic OUT and periodic
 * OUT. Which division is used caps the largest packet each direction can carry, so a port
 * carrying a wide isochronous OUT stream and a port carrying high-speed bulk want opposite
 * divisions. USB_HOST_FIFO_BIAS_DEFAULT leaves the port on the CONFIG_USB_HOST_HW_BUFFER_BIAS
 * choice, which is what a caller that sets nothing gets.
 */
typedef enum {
    USB_HOST_FIFO_BIAS_DEFAULT = 0, /**< Follow the CONFIG_USB_HOST_HW_BUFFER_BIAS Kconfig choice */
    USB_HOST_FIFO_BIAS_BALANCED,    /**< Balanced between the three FIFOs */
    USB_HOST_FIFO_BIAS_IN,          /**< Favour received data */
    USB_HOST_FIFO_BIAS_PERIODIC_OUT,/**< Favour periodic (isochronous, interrupt) OUT */
} usb_host_fifo_bias_t;

typedef struct {
    bool skip_phy_setup;                        /**< If set, the USB Host Library will not configure the USB PHY thus allowing"""

_PUBLIC_CONFIG_ANCHOR = (
    """    unsigned peripheral_map;      /**< Selects the USB peripheral(s) to use."""
)

_PUBLIC_CONFIG_ADDITION = """    usb_host_fifo_bias_t fifo_bias_per_port[SOC_USB_OTG_PERIPH_NUM];
                                  /**< Optional per-port FIFO size biasing. Indexed the same way as
                                       peripheral_map. An entry left at USB_HOST_FIFO_BIAS_DEFAULT
                                       follows the Kconfig choice, which is the behaviour of a
                                       caller that does not set this field at all. Unlike
                                       fifo_settings_custom this is honoured for dual port
                                       applications, since each port resolves its own split from
                                       its own FIFO depth. */
    unsigned peripheral_map;      /**< Selects the USB peripheral(s) to use."""

_PUBLIC_INCLUDE_ANCHOR = """#include "esp_intr_alloc.h"
"""

_PUBLIC_INCLUDE_ADDITION = """#include "esp_intr_alloc.h"
#include "soc/soc_caps.h"
"""

_HCD_HEADER_ANCHOR = """typedef struct {
    hcd_port_callback_t callback;           /**< HCD port event callback */
    void *callback_arg;                     /**< User argument for HCD port callback */
    void *context;                          /**< Context variable used to associate the port with upper layer object */
    const hcd_fifo_settings_t *fifo_config; /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */
    int intr_flags;                         /**< Interrupt flags for HCD interrupt */
} hcd_port_config_t;"""

_HCD_HEADER_REPLACEMENT = """/**
 * @brief Which of the hardware FIFO size biases a port uses
 *
 * Kept numerically identical to usb_host_fifo_bias_t so the value can be passed straight
 * through without a translation table that could drift.
 */
typedef enum {
    HCD_FIFO_BIAS_DEFAULT = 0,   /**< Follow the CONFIG_USB_HOST_HW_BUFFER_BIAS Kconfig choice */
    HCD_FIFO_BIAS_BALANCED,      /**< Balanced between the three FIFOs */
    HCD_FIFO_BIAS_IN,            /**< Favour received data */
    HCD_FIFO_BIAS_PERIODIC_OUT,  /**< Favour periodic (isochronous, interrupt) OUT */
} hcd_fifo_bias_t;

typedef struct {
    hcd_port_callback_t callback;           /**< HCD port event callback */
    void *callback_arg;                     /**< User argument for HCD port callback */
    void *context;                          /**< Context variable used to associate the port with upper layer object */
    const hcd_fifo_settings_t *fifo_config; /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */
    hcd_fifo_bias_t fifo_bias;              /**< Bias used when fifo_config is NULL. HCD_FIFO_BIAS_DEFAULT follows Kconfig. */
    int intr_flags;                         /**< Interrupt flags for HCD interrupt */
} hcd_port_config_t;"""

_HCD_CALC_ANCHOR = """static void  _calculate_fifo_from_bias(port_t *port, const usb_dwc_hal_context_t *hal)
{
    const int otg_dfifo_depth = hal->constant_config.hsphy_type ? 1024 : 256;
    const uint16_t fifo_size_lines = hal->constant_config.fifo_size;

#if CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
    // Prioritize RX FIFO (best for IN-heavy workloads)
    port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
    port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
    port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;

#elif CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
    // Prioritize periodic TX FIFO (useful for high throughput periodic endpoints)
    port->fifo_config.rx_fifo_lines = otg_dfifo_depth / 8 + 2; // 2 extra lines are allocated for status information. See USB-OTG Programming Guide, chapter 2.1.2.1
    port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
    port->fifo_config.ptx_fifo_lines = fifo_size_lines - port->fifo_config.nptx_fifo_lines - port->fifo_config.rx_fifo_lines;

#else // USB_HOST_HW_BUFFER_BIAS_BALANCED
    // Balanced configuration (default)
    port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 4;
    port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
    port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;
#endif
}"""

_HCD_CALC_REPLACEMENT = """static hcd_fifo_bias_t _kconfig_fifo_bias(void)
{
#if CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
    return HCD_FIFO_BIAS_IN;
#elif CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT
    return HCD_FIFO_BIAS_PERIODIC_OUT;
#else
    return HCD_FIFO_BIAS_BALANCED;
#endif
}

static void  _calculate_fifo_from_bias(port_t *port, const usb_dwc_hal_context_t *hal, hcd_fifo_bias_t bias)
{
    const int otg_dfifo_depth = hal->constant_config.hsphy_type ? 1024 : 256;
    const uint16_t fifo_size_lines = hal->constant_config.fifo_size;

    if (bias == HCD_FIFO_BIAS_DEFAULT) {
        bias = _kconfig_fifo_bias();
    }

    switch (bias) {
    case HCD_FIFO_BIAS_IN:
        // Prioritize RX FIFO (best for IN-heavy workloads)
        port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
        port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
        port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;
        break;

    case HCD_FIFO_BIAS_PERIODIC_OUT:
        // Prioritize periodic TX FIFO (useful for high throughput periodic endpoints)
        port->fifo_config.rx_fifo_lines = otg_dfifo_depth / 8 + 2; // 2 extra lines are allocated for status information. See USB-OTG Programming Guide, chapter 2.1.2.1
        port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 16;
        port->fifo_config.ptx_fifo_lines = fifo_size_lines - port->fifo_config.nptx_fifo_lines - port->fifo_config.rx_fifo_lines;
        break;

    default:
        // Balanced configuration (default)
        port->fifo_config.nptx_fifo_lines = otg_dfifo_depth / 4;
        port->fifo_config.ptx_fifo_lines = otg_dfifo_depth / 8;
        port->fifo_config.rx_fifo_lines = fifo_size_lines - port->fifo_config.ptx_fifo_lines - port->fifo_config.nptx_fifo_lines;
        break;
    }
}"""

_HCD_CALL_ANCHOR = """        _calculate_fifo_from_bias(port_obj, port_obj->hal);"""

_HCD_CALL_REPLACEMENT = """        _calculate_fifo_from_bias(port_obj, port_obj->hal, port_config->fifo_bias);"""

_HUB_HEADER_ANCHOR = """    const hcd_fifo_settings_t *fifo_config;         /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */"""

_HUB_HEADER_REPLACEMENT = """    const hcd_fifo_settings_t *fifo_config;         /**< Optional pointer to custom FIFO config. If NULL, default configuration is used. */
    hcd_fifo_bias_t fifo_bias[HCD_NUM_PORTS];       /**< Per-port bias used where fifo_config is NULL. */"""

_HUB_PORT_ANCHOR = """                .intr_flags = hub_config->intr_flags,
                .fifo_config = hub_config->fifo_config,"""

_HUB_PORT_REPLACEMENT = """                .intr_flags = hub_config->intr_flags,
                .fifo_config = hub_config->fifo_config,
                .fifo_bias = hub_config->fifo_bias[i],"""

_HOST_WARN_ANCHOR = """        .intr_flags = config->intr_flags,
        .fifo_config = NULL,
    };"""

_HOST_WARN_REPLACEMENT = """        .intr_flags = config->intr_flags,
        .fifo_config = NULL,
    };

    // Per-port FIFO biasing. Unlike fifo_settings_custom this is meaningful with more than
    // one port: the bias only selects which of the three splits a port uses, and each port
    // resolves that split against its own FIFO depth in _calculate_fifo_from_bias().
    for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {
        hub_config.fifo_bias[i] = (hcd_fifo_bias_t)config->fifo_bias_per_port[i];
    }"""

_LS_PREAMBLE_ANCHOR = """static inline bool _buffer_check_done(pipe_t *pipe)
{
    // Only control transfers need to be continued
    if (pipe->ep_char.type != USB_DWC_XFER_TYPE_CTRL) {
        return true;
    }
    // The HW can't handle two transactions with preamble in one frame.
    // TODO: IDF-12986
    if (pipe->ep_char.ls_via_fs_hub) {
        esp_rom_delay_us(1000);
    }"""

_LS_PREAMBLE_REPLACEMENT = """/**
 * @brief Wait for the frame counter to advance before the next PREamble transaction
 *
 * The controller cannot issue two PRE-qualified transactions in the same frame. Sleeping
 * for one frame's worth of microseconds only works when the stage completed near the start
 * of a frame: a completion just before the counter rolls over is followed by another
 * transaction in the very next frame, which is the case this is meant to prevent. Watch the
 * counter itself instead.
 *
 * The bound is what keeps this safe to call from an interrupt: a stopped frame clock, which
 * is what a disconnect during a control transfer looks like, must not wedge the handler. A
 * full-speed frame is 1 ms, so 3 ms is well past any legitimate transition.
 */
static inline void _wait_next_preamble_frame(pipe_t *pipe)
{
    const uint32_t start_frame = usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal);
    for (int waited_us = 0; waited_us < 3000; waited_us += 10) {
        if (usb_dwc_hal_port_get_cur_frame_num(pipe->port->hal) != start_frame) {
            return;
        }
        esp_rom_delay_us(10);
    }
}

static inline bool _buffer_check_done(pipe_t *pipe)
{
    // Only control transfers need to be continued
    if (pipe->ep_char.type != USB_DWC_XFER_TYPE_CTRL) {
        return true;
    }
    // The HW can't handle two transactions with preamble in one frame.
    if (pipe->ep_char.ls_via_fs_hub) {
        _wait_next_preamble_frame(pipe);
    }"""

_LS_EP0_ANCHOR = """    ep_char->ls_via_fs_hub = 0;
    if (pipe_idx > 0) {
        // TODO: remove warning after IDF-12986
        if (port_speed == USB_SPEED_FULL && pipe_config->dev_speed == USB_SPEED_LOW) {
            ESP_LOGW(HCD_DWC_TAG, "Low-speed, extra delay will be applied in ISR");
            ep_char->ls_via_fs_hub = 1;
        }
    }"""

_LS_EP0_REPLACEMENT = """    // A low-speed device reached through a full-speed hub needs a PREamble before every
    // transaction, the default control pipe included. Excluding pipe index 0 left that pipe
    // without it, so the first GET_DESCRIPTOR of enumeration went out unqualified and the
    // device never answered -- no endpoint of such a device could ever be reached, which is
    // why the exclusion was not noticed. A device on the root port is unaffected either way:
    // there the port speed is LOW, not FULL.
    ep_char->ls_via_fs_hub = 0;
    if (port_speed == USB_SPEED_FULL && pipe_config->dev_speed == USB_SPEED_LOW) {
        ESP_LOGD(HCD_DWC_TAG, "Low-speed device behind a full-speed hub, PREamble enabled");
        ep_char->ls_via_fs_hub = 1;
    }"""

# (relative path, anchor, replacement) applied in order, each exactly once.
PATCHES = (
    ("include/usb/usb_host.h", _PUBLIC_INCLUDE_ANCHOR, _PUBLIC_INCLUDE_ADDITION),
    ("include/usb/usb_host.h", _PUBLIC_HEADER_ANCHOR, _PUBLIC_HEADER_ADDITION),
    ("include/usb/usb_host.h", _PUBLIC_CONFIG_ANCHOR, _PUBLIC_CONFIG_ADDITION),
    ("private_include/hcd.h", _HCD_HEADER_ANCHOR, _HCD_HEADER_REPLACEMENT),
    ("private_include/hub.h", _HUB_HEADER_ANCHOR, _HUB_HEADER_REPLACEMENT),
    ("src/hcd_dwc.c", _HCD_CALC_ANCHOR, _HCD_CALC_REPLACEMENT),
    ("src/hcd_dwc.c", _HCD_CALL_ANCHOR, _HCD_CALL_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_PREAMBLE_ANCHOR, _LS_PREAMBLE_REPLACEMENT),
    ("src/hcd_dwc.c", _LS_EP0_ANCHOR, _LS_EP0_REPLACEMENT),
    ("src/hub.c", _HUB_PORT_ANCHOR, _HUB_PORT_REPLACEMENT),
    ("src/usb_host.c", _HOST_WARN_ANCHOR, _HOST_WARN_REPLACEMENT),
)

# Identifies an existing patched copy. It has to change whenever anything about that copy
# would change, or a copy left over from an earlier revision is trusted and silently used:
# hence a digest over the patch bodies themselves rather than a hand-maintained number.
PATCH_STAMP = "v1:{}:{}".format(
    USB_COMPONENT_VERSION,
    hashlib.sha256(
        "\0".join(part for patch in PATCHES for part in patch).encode()
    ).hexdigest()[:16],
)
