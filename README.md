# Nova Light — ESP32 Smart Light

An ESP32-WROOM-32 relay controller with a polished local dashboard, adjustable daily/overnight schedule, Asia/Kolkata NTP time, saved settings, and activity history.

## Files

- [GitHub Pages website source](docs/index.html)
- [Website setup guide](docs/setup.html)
- [Complete Arduino project and dashboard](outputs/NovaLightESP32/README.md)
- [ESP32 firmware](outputs/NovaLightESP32/firmware/NovaLight/NovaLight.ino)
- [Standalone dashboard source](outputs/NovaLightESP32/dashboard.html)

The website in `docs/` is a public project page. The live control panel is served by the ESP32 from your home network. The website provides a local-IP link to that dashboard; it does not claim a live connection.

## Publish with GitHub Pages

1. Add this project to a GitHub repository.
2. On GitHub, open **Settings → Pages**.
3. Under **Build and deployment**, choose **Deploy from a branch**.
4. Select your default branch and the **`/docs`** folder, then save.
5. Wait for the Pages deployment to finish. GitHub displays the public site URL in the Pages settings.

GitHub Pages publishes the selected source folder when changes reach the selected branch. The Pages website is public, so keep Wi-Fi credentials in the ignored local `secrets.h` file, not in the repository. See [GitHub's Pages publishing documentation](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

## Local control constraint

GitHub Pages is delivered over HTTPS. Browsers block an HTTPS page from making active `fetch()` calls to the ESP32's plain-HTTP local API. Nova Light therefore keeps control on the ESP32 and opens its dashboard directly by local address. This avoids mixed-content calls and needs no external backend.

