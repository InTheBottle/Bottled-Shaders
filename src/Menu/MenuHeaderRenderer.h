#pragma once

/**
 * @brief Renders the menu chrome: the command bar above the page and the status bar below it.
 *
 * The header carries no imagery beyond the logo. When the window is docked the title bar
 * already names the window, so only the commands are drawn.
 */
class MenuHeaderRenderer
{
public:
	/**
	 * @brief Renders the menu header area.
	 *
	 * @param isDocked True if the menu window is docked into a tab bar.
	 */
	static void RenderHeader(bool isDocked);

	static float GetStatusBarHeight();
	static void RenderStatusBar();
};
