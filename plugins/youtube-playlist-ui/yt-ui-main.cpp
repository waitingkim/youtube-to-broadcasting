#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>

#include "playlist-dock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("youtube-playlist-ui", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Dock for controlling YouTube playlist sources";
}

static QPointer<PlaylistDock> dock_widget;

/* OBS adds plugin docks hidden.  Show ours once, on the first launch after
 * installation, so it can be found; afterwards OBS remembers the layout. */
static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	char *marker = obs_module_config_path("dock-shown");
	if (marker && !os_file_exists(marker) && dock_widget && dock_widget->parentWidget()) {
		/* dock it into the main window (OBS adds plugin docks floating) */
		if (auto *qdock = qobject_cast<QDockWidget *>(dock_widget->parentWidget()))
			qdock->setFloating(false);
		dock_widget->parentWidget()->setVisible(true);
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		os_quick_write_utf8_file(marker, "1", 1, false);
	}
	bfree(marker);
}

bool obs_module_load(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new PlaylistDock(main);
	if (!obs_frontend_add_dock_by_id("youtube-playlist-dock", obs_module_text("DockTitle"), dock)) {
		delete dock;
		return false;
	}
	dock_widget = dock;
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
}
