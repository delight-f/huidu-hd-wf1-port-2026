#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Runtime panel-configuration cycler for bench bring-up.
//
// The HD-WF1 exposes a single push button, wired to GPIO 11 (CONFIG_BUTTON_PIN
// - the same button the firmware reads at boot to force config mode). While the
// cycler is running, a short press advances to the next candidate panel
// driver/timing combination, writes it to NVS and reboots.
//
// Candidate configurations are identified at boot by a solid fill colour. Solid
// fills render correctly even when the panel is otherwise mis-configured (a
// full-screen fill is invariant under row/timing errors), so the colour is a
// reliable "which config is live" indicator where on-screen text is not.
//
// The active index, name and colour are also logged and shown on /diag.

void panel_sweep_start(void);

// Log the active configuration and fill the panel with its indicator colour.
void panel_sweep_indicator(void);

// Active configuration index (0 = board defaults).
int panel_sweep_index(void);

// Raw GPIO levels of the candidate button pins, for bench diagnosis via /diag.
int panel_sweep_button_level(void);  // CONFIG_BUTTON_PIN (GPIO11 on the WF1)
int panel_sweep_gpio0_level(void);   // GPIO0

#ifdef __cplusplus
}
#endif
