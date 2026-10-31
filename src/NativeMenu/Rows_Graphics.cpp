#include "NativeMenu/NativeMenu.h"

#include "Globals.h"
#include "I18n/I18n.h"
#include "Menu.h"

#define I18N_KEY_PREFIX "native_menu.graphics."

namespace
{
	void __stdcall OpenCommunityShadersMenu()
	{
		if (globals::menu)
			globals::menu->RequestOpen();
	}

	void __stdcall RebindMenuHotkey()
	{
		if (globals::menu)
			globals::menu->BeginToggleKeyRebind();
	}

	void __stdcall GetMenuHotkeyText(char* buffer, int size)
	{
		auto* menu = globals::menu;
		if (menu && menu->IsRebindingToggleKey()) {
			std::snprintf(buffer, size, "%s", T(TKEY("hotkey_capture"), "Press a key... (Esc cancels)"));
			return;
		}

		const auto key = menu ? menu->GetToggleKeyName() : std::string{};
		std::snprintf(buffer, size, "%s: %s", T(TKEY("hotkey"), "Menu Hotkey"), key.c_str());
	}
}

namespace NativeMenu
{
	std::vector<Row> GraphicsRows()
	{
		std::vector<Row> rows{
			NATIVE_MENU_HEADING(T(TKEY("heading"), "Bottled Shaders")),

			Button(T(TKEY("open_menu"), "Open Bottled Shaders Menu"), &OpenCommunityShadersMenu,
				T(TKEY("open_menu_desc"), "Opens the full Bottled Shaders settings window.")),

			Button(T(TKEY("hotkey"), "Menu Hotkey"), &RebindMenuHotkey,
				T(TKEY("hotkey_desc"), "Select this, then press the key or combo that opens the Bottled Shaders menu."),
				nullptr, &GetMenuHotkeyText),
		};

		AppendRows(rows, HDRRows());
		AppendRows(rows, SSGIRows());
		AppendRows(rows, UpscalingRows());

		return rows;
	}
}

#undef I18N_KEY_PREFIX
