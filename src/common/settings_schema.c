#include <disc/settings.h>

#define B(k, d, pg, lb) { .key = k, .type = SET_BOOL, .def = d, .page = pg, .label = lb }
#define I(k, d, lo, hi, st, pg, lb, un) { .key = k, .type = SET_INT, .def = d, .min = lo, .max = hi, .step = st, .page = pg, .label = lb, .unit = un }
#define E(k, d, ch, pg, lb) { .key = k, .type = SET_ENUM, .def = d, .choices = ch, .page = pg, .label = lb }
#define S(k, d, pg, lb) { .key = k, .type = SET_STR, .def = d, .page = pg, .label = lb }

#define PROFILE(p, dim, off, idle, wifi, bt, anim) \
	I("power." p ".dim_s", dim, 0, 600, 5, "power." p, "Dim screen after", "s"), \
	I("power." p ".off_s", off, 0, 3600, 5, "power." p, "Screen off after", "s"), \
	I("power." p ".idle_off_min", idle, 0, 240, 5, "power." p, "Power off when idle", "min"), \
	I("power." p ".wifi_idle_min", wifi, 0, 240, 5, "power." p, "Wi-Fi off when unused", "min"), \
	I("power." p ".bt_idle_min", bt, 0, 240, 5, "power." p, "Bluetooth off when unused", "min"), \
	B("power." p ".anim", anim, "power." p, "Animations")

#define ACTIONS "none=Nothing|play_pause=Play / pause|next=Next track|prev=Previous track|vol_up=Volume up|" \
	"vol_down=Volume down|seek_fwd=Seek forward|seek_back=Seek back|screen=Screen on / off|lock=Lock|" \
	"order=Cycle play order|bt=Bluetooth on / off|wifi=Wi-Fi on / off|sleep=Sleep timer"

#define KEY(k, p, on, off, lb) \
	E("keys." k "." p ".on", on, ACTIONS, "buttons", lb), \
	E("keys." k "." p ".off", off, ACTIONS, "buttons", lb " (screen off)")

#define PROFILES "saver=Saver|balanced=Balanced|performance=Performance|custom=Custom"

const struct set_def set_defs[] = {
	I("ui.brightness", "30", 1, 40, 1, "display", "Brightness", NULL),
	E("ui.rotation", "0", "0=0°|90=90°|180=180°|270=270°", "display", "Screen rotation"),
	E("ui.font", "medium", "small=Small|medium=Medium|large=Large", "display", "Font size"),
	E("ui.accent", "teal", "teal=Teal|cyan=Cyan|blue=Blue|purple=Purple|pink=Pink|red=Red|orange=Orange|amber=Amber|green=Green",
	  "display", "Accent color"),
	E("ui.cover_mode", "static", "none=None|static=Static|rotating=Rotating|vinyl=Vinyl", "display", "Cover"),
	I("ui.anim_fps", "12", 4, 30, 1, "display", "Animation frame rate", "fps"),
	E("ui.start_screen", "last", "last=Last screen|home=Home|folders=Folders|nowplaying=Now playing", "display", "Start screen"),
	B("ui.index", "1", "display", "Letter index in lists"),
	E("ui.marquee", "all", "all=All long names|active=Playing item only|off=Off", "display", "Scroll long names"),
	I("ui.back_px", "60", 30, 120, 5, "display", "Back swipe distance", "px"),
	I("ui.edge_px", "24", 12, 48, 2, "display", "Edge zone width", "px"),

	B("lock.enabled", "1", "lock", "Lock screen"),
	B("lock.song", "1", "lock", "Show song"),
	B("lock.cover", "0", "lock", "Show cover"),
	B("lock.date", "1", "lock", "Show date"),
	B("lock.battery", "1", "lock", "Show battery"),
	E("lock.unsynced", "mark", "hide=Hide|mark=Show, marked|show=Show", "lock", "Time before clock sync"),

	S("time.zone", "UTC", "time", "Timezone"),
	B("time.24h", "1", "time", "24-hour clock"),
	B("time.ntp", "1", "time", "Sync over Wi-Fi (NTP)"),

	E("play.order", "seq", "seq=Sequential|random=Shuffle|noloop=Sequential, no loop|single=Repeat one|once=Single track",
	  "playback", "Play order"),
	E("play.resume", "position", "off=Off|song=Song|position=Position", "playback", "Resume after power-on"),
	B("play.recursive", "0", "playback", "Folder play includes subfolders"),
	B("play.through_folders", "1", "playback", "Play through folders"),
	E("play.replaygain", "off", "off=Off|track=Track|album=Album|auto=Auto", "playback", "ReplayGain"),
	E("play.sleep_action", "pause", "pause=Pause|poweroff=Power off", "playback", "Sleep timer action"),
	E("pl.group", "album", "none=No grouping|album=Album|folder=Folder|both=Album and folder", "playback",
	  "Group playlist and queue by"),
	S("pl.sort_presets",
	  "Default=YEAR / ALBUM / DIRECTORY / DISCNUMBER / TRACKNUMBER / FILENAME / TITLE;"
	  "Artist=ARTIST / YEAR / ALBUM / DISCNUMBER / TRACKNUMBER;Newest first=-YEAR / ALBUM / DISCNUMBER / TRACKNUMBER;"
	  "Folder=DIRECTORY / FILENAME;Title=TITLE;Title, descending=-TITLE",
	  "playback", "Sort presets (Name=CRITERIA; …)"),

	I("audio.vol_step", "2", 1, 10, 1, "audio", "Volume step", "%"),
	I("audio.vol_max", "100", 10, 100, 1, "audio", "Maximum volume", "%"),
	I("audio.dac_range", "80", 30, 120, 5, "audio", "Volume range", "dB"),
	I("audio.balance", "0", -20, 20, 1, "audio", "Balance (+ = right louder)", "0.5 dB"),
	E("audio.filter", "fast_ll",
	  "fast_ll=Fast, low latency|slow_ll=Slow, low latency|slow_pc=Slow, phase comp.|fast_pc=Fast, phase comp.|"
	  "nos=Non-oversampling|wideband=Wideband flat",
	  "audio", "DAC filter"),
	B("audio.dre", "0", "audio", "Dynamic range enhancement"),
	B("audio.spdif", "0", "audio", "S/PDIF output (3.5 mm)"),
	B("audio.pause_bt_loss", "1", "audio", "Pause when Bluetooth disconnects"),
	B("audio.pause_jack_out", "1", "audio", "Pause when headphones unplugged"),
	I("audio.dac_idle_off", "0", 0, 600, 10, "audio", "DAC power-down when idle (experimental)", "s"),

	S("eq.dac", "", "", "Equalizer: headphones"),
	S("eq.spdif", "", "", "Equalizer: S/PDIF"),
	S("eq.bt", "", "", "Equalizer: Bluetooth"),
	B("eq.bt_per_device", "1", "", "Equalizer per Bluetooth device"),

	E("lib.scan", "card", "manual=Manual|card=When the card changes|mount=On every mount", "library", "Scan library"),
	B("lib.scan_after_usb", "1", "library", "Scan after USB storage"),
	E("lib.artist_tag", "albumartist", "artist=Artist|albumartist=Album artist", "library", "Artist list by"),
	E("lib.playlist_sort", "folder", "folder=Folder order|tags=Tags (disc, track)", "library", "New playlist order"),
	B("lib.hidden", "0", "library", "Show hidden files"),

	E("power.battery", "balanced", PROFILES, "power", "Profile on battery"),
	E("power.charger", "performance", PROFILES, "power", "Profile on charger"),
	PROFILE("saver", "10", "20", "10", "5", "10", "0"),
	PROFILE("balanced", "20", "45", "30", "15", "30", "1"),
	PROFILE("performance", "60", "300", "0", "0", "0", "1"),
	PROFILE("custom", "30", "60", "60", "30", "0", "1"),
	I("power.lowbat_warn", "10", 0, 50, 1, "power", "Low battery warning", "%"),
	I("power.lowbat_off", "3", 0, 20, 1, "power", "Power off at", "%"),
	I("power.lowbat_mv", "3400", 3000, 3700, 50, "power", "Power off below", "mV"),

	I("battery.limit", "100", 50, 100, 5, "battery", "Charge limit", "%"),
	I("battery.resume_drop", "5", 1, 20, 1, "battery", "Resume charging after a drop of", "%"),
	E("battery.current", "normal", "normal=Normal (1.3 A)|reduced=Reduced (0.6 A)", "battery", "Charge current"),
	I("battery.temp_max", "0", 0, 60, 1, "battery", "Stop charging above (0 = off)", "°C"),

	I("keys.double_ms", "350", 150, 800, 25, "", "Double-press window", "ms"),
	I("keys.long_ms", "700", 300, 2000, 50, "", "Long-press time", "ms"),
	I("keys.repeat_ms", "150", 50, 1000, 25, "buttons", "Hold repeat interval", "ms"),
	I("keys.power_long_ms", "2500", 1500, 4000, 100, "", "Power-off hold time", "ms"),
	I("keys.seek_s", "10", 2, 60, 1, "buttons", "Seek step", "s"),
	KEY("play", "short", "play_pause", "play_pause", "Play: press"),
	KEY("play", "double", "next", "next", "Play: double press"),
	KEY("play", "long", "prev", "prev", "Play: hold"),
	KEY("volup", "short", "vol_up", "vol_up", "Vol+: press"),
	KEY("volup", "double", "next", "next", "Vol+: double press"),
	KEY("volup", "long", "vol_up", "vol_up", "Vol+: hold"),
	KEY("voldown", "short", "vol_down", "vol_down", "Vol−: press"),
	KEY("voldown", "double", "prev", "prev", "Vol−: double press"),
	KEY("voldown", "long", "vol_down", "vol_down", "Vol−: hold"),
	KEY("power", "short", "screen", "screen", "Power: press"),
	KEY("power", "double", "none", "none", "Power: double press"),

	B("wifi.enabled", "0", "wifi", "Wi-Fi"),
	B("net.mpd", "1", "wifi", "MPD over network"),
	S("net.mpd_password", "", "wifi", "MPD password"),
	B("net.ssh", "1", "wifi", "SSH / SFTP"),

	B("bt.enabled", "0", "bluetooth", "Bluetooth"),
	B("bt.autoreconnect", "1", "bluetooth", "Reconnect automatically"),
	B("bt.battery", "1", "bluetooth", "Headphone battery"),
};

const int set_ndefs = sizeof(set_defs) / sizeof(set_defs[0]);
