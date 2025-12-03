#include <cstdlib>
#include <string>

#include <switch.h>
#include <curl/curl.h>

#include "util.h"
#include "config.h"
#include "lang.h"
#include "gui.h"
#include "logger.h"
// #include "dbglogger.h"

static SetLanguage lang;
static void LogCurlInfo()
{
  curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
  if (!info)
  {
    Logger::Log("curl: version info not available");
    return;
  }

  std::string line = "curl: ";
  if (info->version)
    line += info->version;

  line += " features:";
  if (info->features & CURL_VERSION_SSL)
    line += " ssl";
  if (info->features & CURL_VERSION_LIBZ)
    line += " libz";
  if (info->features & CURL_VERSION_IPV6)
    line += " ipv6";
  if (info->features & CURL_VERSION_HTTP2)
    line += " http2";
#ifdef CURL_VERSION_TLSAUTH_SRP
  if (info->features & CURL_VERSION_TLSAUTH_SRP)
    line += " tls-srp";
#endif
#ifdef CURL_VERSION_ALTSVC
  if (info->features & CURL_VERSION_ALTSVC)
    line += " altsvc";
#endif
#ifdef CURL_VERSION_BROTLI
  if (info->features & CURL_VERSION_BROTLI)
    line += " brotli";
#endif
  // Zstd flag is not available on all libcurl versions.
#ifdef CURL_VERSION_ZSTD
  if (info->features & CURL_VERSION_ZSTD)
    line += " zstd";
#endif

  line += " protocols:";
  if (info->protocols)
  {
    for (int i = 0; info->protocols[i]; ++i)
    {
      line += " ";
      line += info->protocols[i];
    }
  }
  Logger::Log(line);
}

namespace Services
{
  int Init(void)
  {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    // Boost CPU and optimize wireless for better transfer performance.
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);
    // Keep the console awake while this app is running so long downloads
    // are not interrupted by auto-sleep.
    appletSetAutoSleepDisabled(true);

    plInitialize(PlServiceType_User);
    romfsInit();
    socketInitializeDefault();
    nxlinkStdio();
    setInitialize();
    Logger::Init();
    Logger::Log("App start");
    LogCurlInfo();
    remoteclient = nullptr;
    u64 lang_code = -1;
    setGetSystemLanguage(&lang_code);
    setMakeLanguage(lang_code, &lang);
    setExit();

    CONFIG::LoadConfig();
    Lang::SetTranslation(lang);
    FontType fontType = FONT_TYPE_LATIN;
    if (strcasecmp(language, "Simplified Chinese") == 0 || lang == 6 || lang == 15)
    {
      fontType = FONT_TYPE_SIMPLIFIED_CHINESE;
    }
    else if (strcasecmp(language, "Traditional Chinese") == 0 || lang == 11 || lang == 16)
    {
      fontType = FONT_TYPE_TRADITIONAL_CHINESE;
    }
    else if (strcasecmp(language, "Korean") == 0 || lang == 7)
    {
      fontType = FONT_TYPE_KOREAN;
    }
    else if (strcasecmp(language, "Japanese") == 0 || strcasecmp(language, "Ryukyuan") == 0 || lang == 0)
    {
      fontType = FONT_TYPE_JAPANESE;
    }
    else if (strcasecmp(language, "Thai") == 0)
    {
      fontType = FONT_TYPE_THAI;
    }
    else if (strcasecmp(language, "Arabic") == 0)
    {
      fontType = FONT_TYPE_ARABIC;
    }
    else if (strcasecmp(language, "Vietnamese") == 0)
    {
      fontType = FONT_TYPE_VIETNAMESE;
    }
    else if (strcasecmp(language, "Greek") == 0)
    {
      fontType = FONT_TYPE_GREEK;
    }

    GUI::Init(fontType);
    plExit();

    return 0;
  }

  void Exit(void)
  {
    if (remoteclient != nullptr)
    {
      remoteclient->Quit();
      delete remoteclient;
      remoteclient = nullptr;
    }
    curl_global_cleanup();
    GUI::Exit();
    romfsExit();
    socketExit();
  }
} // namespace Services

int main(int argc, char* argv[])
{
  Services::Init();
	// dbglogger_init();
	// dbglogger_log("If you see this you've set up dbglogger correctly.");

  GUI::RenderLoop();

  Services::Exit();
  Logger::Log("App exit");
  return 0;
}
