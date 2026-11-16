#include "boo/localization.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace boo {

namespace {

struct LanguageTable {
    const char* tag;
    uint16_t primaryLang;
    bool traditionalChinese;
    const wchar_t* text[static_cast<size_t>(UiText::Count)]; // UnsupportedGpuBody, UnsupportedGpuTitle, GraphicsFailureBody
};

// "Hardware Orchestra", "Direct3D 12" and the title stay as they are in every language.
// English must stay first: it is the fallback.
const LanguageTable kLanguages[] = {
    {"en-US", LANG_ENGLISH, false, {
        L"BOO SAH SAH requires a compatible Direct3D 12 device.\nSee logs for Hardware Orchestra diagnostics.",
        L"BOO SAH SAH — Unsupported Graphics Device",
        L"Graphics playback could not be restored.\nThe audio and timeline remain available. Close the show and restart to retry."}},
    {"fr-FR", LANG_FRENCH, false, {
        L"BOO SAH SAH nécessite un périphérique compatible Direct3D 12.\nConsultez les journaux pour les diagnostics Hardware Orchestra.",
        L"BOO SAH SAH — Périphérique graphique non pris en charge",
        L"La lecture graphique n'a pas pu être rétablie.\nL'audio et la chronologie restent disponibles. Fermez l'application et relancez-la pour réessayer."}},
    {"de-DE", LANG_GERMAN, false, {
        L"BOO SAH SAH benötigt ein Direct3D 12-kompatibles Gerät.\nDiagnosedaten zu Hardware Orchestra finden Sie in den Protokollen.",
        L"BOO SAH SAH — Grafikgerät wird nicht unterstützt",
        L"Die Grafikwiedergabe konnte nicht wiederhergestellt werden.\nAudio und Zeitleiste bleiben verfügbar. Schließen Sie die App und starten Sie sie neu, um es erneut zu versuchen."}},
    {"es-ES", LANG_SPANISH, false, {
        L"BOO SAH SAH requiere un dispositivo compatible con Direct3D 12.\nConsulta los registros para ver el diagnóstico de Hardware Orchestra.",
        L"BOO SAH SAH — Dispositivo gráfico no compatible",
        L"No se pudo restaurar la reproducción de gráficos.\nEl audio y la línea de tiempo siguen disponibles. Cierra la aplicación y vuelve a iniciarla para reintentarlo."}},
    {"it-IT", LANG_ITALIAN, false, {
        L"BOO SAH SAH richiede un dispositivo compatibile con Direct3D 12.\nConsulta i registri per la diagnostica di Hardware Orchestra.",
        L"BOO SAH SAH — Dispositivo grafico non supportato",
        L"Impossibile ripristinare la riproduzione grafica.\nL'audio e la timeline restano disponibili. Chiudi l'app e riavviala per riprovare."}},
    {"pt-BR", LANG_PORTUGUESE, false, {
        L"O BOO SAH SAH requer um dispositivo compatível com Direct3D 12.\nConsulte os logs para ver o diagnóstico do Hardware Orchestra.",
        L"BOO SAH SAH — Dispositivo gráfico não compatível",
        L"Não foi possível restaurar a reprodução gráfica.\nO áudio e a linha do tempo continuam disponíveis. Feche o aplicativo e reinicie para tentar novamente."}},
    {"ja-JP", LANG_JAPANESE, false, {
        L"BOO SAH SAH には Direct3D 12 対応デバイスが必要です。\nHardware Orchestra の診断情報はログを確認してください。",
        L"BOO SAH SAH — サポートされていないグラフィックス デバイス",
        L"グラフィックスの再生を復元できませんでした。\nオーディオとタイムラインは引き続き利用できます。アプリを閉じて再起動し、もう一度お試しください。"}},
    {"ko-KR", LANG_KOREAN, false, {
        L"BOO SAH SAH를 실행하려면 Direct3D 12 호환 장치가 필요합니다.\nHardware Orchestra 진단 정보는 로그를 확인하세요.",
        L"BOO SAH SAH — 지원되지 않는 그래픽 장치",
        L"그래픽 재생을 복원할 수 없습니다.\n오디오와 타임라인은 계속 사용할 수 있습니다. 앱을 닫고 다시 시작하여 다시 시도하세요."}},
    {"zh-CN", LANG_CHINESE, false, {
        L"BOO SAH SAH 需要兼容 Direct3D 12 的设备。\n请查看日志中的 Hardware Orchestra 诊断信息。",
        L"BOO SAH SAH — 不支持的图形设备",
        L"无法恢复图形播放。\n音频和时间线仍然可用。请关闭应用并重新启动以重试。"}},
    {"zh-TW", LANG_CHINESE, true, {
        L"BOO SAH SAH 需要相容 Direct3D 12 的裝置。\n請查看記錄檔中的 Hardware Orchestra 診斷資訊。",
        L"BOO SAH SAH — 不支援的圖形裝置",
        L"無法還原圖形播放。\n音訊和時間軸仍可使用。請關閉應用程式並重新啟動以重試。"}},
    {"ru-RU", LANG_RUSSIAN, false, {
        L"Для BOO SAH SAH требуется устройство с поддержкой Direct3D 12.\nДиагностику Hardware Orchestra смотрите в журналах.",
        L"BOO SAH SAH — Графическое устройство не поддерживается",
        L"Не удалось восстановить воспроизведение графики.\nЗвук и временная шкала по-прежнему доступны. Закройте приложение и запустите его снова, чтобы повторить попытку."}},
    {"pl-PL", LANG_POLISH, false, {
        L"BOO SAH SAH wymaga urządzenia zgodnego z Direct3D 12.\nDiagnostykę Hardware Orchestra znajdziesz w dziennikach.",
        L"BOO SAH SAH — Nieobsługiwane urządzenie graficzne",
        L"Nie udało się przywrócić odtwarzania grafiki.\nDźwięk i oś czasu są nadal dostępne. Zamknij aplikację i uruchom ją ponownie, aby spróbować jeszcze raz."}},
    {"nl-NL", LANG_DUTCH, false, {
        L"BOO SAH SAH vereist een apparaat dat compatibel is met Direct3D 12.\nRaadpleeg de logboeken voor de diagnose van Hardware Orchestra.",
        L"BOO SAH SAH — Grafisch apparaat niet ondersteund",
        L"Het afspelen van beelden kon niet worden hersteld.\nAudio en tijdlijn blijven beschikbaar. Sluit de app en start deze opnieuw om het nogmaals te proberen."}},
    {"tr-TR", LANG_TURKISH, false, {
        L"BOO SAH SAH, Direct3D 12 uyumlu bir cihaz gerektirir.\nHardware Orchestra tanılama bilgileri için günlüklere bakın.",
        L"BOO SAH SAH — Desteklenmeyen Grafik Cihazı",
        L"Grafik oynatma geri yüklenemedi.\nSes ve zaman çizelgesi kullanılabilir durumda. Yeniden denemek için uygulamayı kapatıp yeniden başlatın."}},
    {"sv-SE", LANG_SWEDISH, false, {
        L"BOO SAH SAH kräver en Direct3D 12-kompatibel enhet.\nSe loggarna för diagnostik från Hardware Orchestra.",
        L"BOO SAH SAH — Grafikenheten stöds inte",
        L"Grafikuppspelningen kunde inte återställas.\nLjudet och tidslinjen är fortfarande tillgängliga. Stäng appen och starta om den för att försöka igen."}},
    {"ar-SA", LANG_ARABIC, false, {
        L"يتطلب BOO SAH SAH جهازًا متوافقًا مع Direct3D 12.\nراجع السجلات للحصول على تشخيصات Hardware Orchestra.",
        L"BOO SAH SAH — جهاز رسومات غير مدعوم",
        L"تعذّر استعادة تشغيل الرسومات.\nلا يزال الصوت والمخطط الزمني متاحين. أغلق التطبيق وأعد تشغيله للمحاولة مرة أخرى."}},
    {"hi-IN", LANG_HINDI, false, {
        L"BOO SAH SAH के लिए Direct3D 12 संगत डिवाइस आवश्यक है।\nHardware Orchestra डायग्नोस्टिक्स के लिए लॉग देखें।",
        L"BOO SAH SAH — असमर्थित ग्राफ़िक्स डिवाइस",
        L"ग्राफ़िक्स प्लेबैक पुनर्स्थापित नहीं किया जा सका।\nऑडियो और टाइमलाइन अब भी उपलब्ध हैं। फिर से प्रयास करने के लिए ऐप बंद करके दोबारा शुरू करें।"}},
    {"th-TH", LANG_THAI, false, {
        L"BOO SAH SAH ต้องใช้อุปกรณ์ที่รองรับ Direct3D 12\nดูข้อมูลการวินิจฉัยของ Hardware Orchestra ได้ในบันทึก",
        L"BOO SAH SAH — ไม่รองรับอุปกรณ์กราฟิกนี้",
        L"ไม่สามารถกู้คืนการเล่นกราฟิกได้\nเสียงและไทม์ไลน์ยังคงใช้งานได้ ปิดแอปแล้วเปิดใหม่เพื่อลองอีกครั้ง"}},
    {"vi-VN", LANG_VIETNAMESE, false, {
        L"BOO SAH SAH yêu cầu thiết bị tương thích với Direct3D 12.\nXem nhật ký để biết thông tin chẩn đoán của Hardware Orchestra.",
        L"BOO SAH SAH — Thiết bị đồ họa không được hỗ trợ",
        L"Không thể khôi phục việc phát đồ họa.\nÂm thanh và dòng thời gian vẫn khả dụng. Hãy đóng ứng dụng và khởi động lại để thử lại."}},
    {"id-ID", LANG_INDONESIAN, false, {
        L"BOO SAH SAH memerlukan perangkat yang kompatibel dengan Direct3D 12.\nLihat log untuk diagnostik Hardware Orchestra.",
        L"BOO SAH SAH — Perangkat Grafis Tidak Didukung",
        L"Pemutaran grafis tidak dapat dipulihkan.\nAudio dan lini masa tetap tersedia. Tutup aplikasi lalu mulai ulang untuk mencoba lagi."}},
    {"cs-CZ", LANG_CZECH, false, {
        L"BOO SAH SAH vyžaduje zařízení kompatibilní s Direct3D 12.\nDiagnostiku Hardware Orchestra najdete v protokolech.",
        L"BOO SAH SAH — Nepodporované grafické zařízení",
        L"Přehrávání grafiky se nepodařilo obnovit.\nZvuk a časová osa zůstávají k dispozici. Zavřete aplikaci a spusťte ji znovu."}},
    {"uk-UA", LANG_UKRAINIAN, false, {
        L"Для BOO SAH SAH потрібен пристрій із підтримкою Direct3D 12.\nДіагностику Hardware Orchestra дивіться в журналах.",
        L"BOO SAH SAH — Графічний пристрій не підтримується",
        L"Не вдалося відновити відтворення графіки.\nЗвук і часова шкала залишаються доступними. Закрийте застосунок і запустіть його знову, щоб повторити спробу."}},
    {"da-DK", LANG_DANISH, false, {
        L"BOO SAH SAH kræver en Direct3D 12-kompatibel enhed.\nSe logfilerne for diagnosticering fra Hardware Orchestra.",
        L"BOO SAH SAH — Grafikenheden understøttes ikke",
        L"Grafikafspilningen kunne ikke gendannes.\nLyd og tidslinje er stadig tilgængelige. Luk appen, og start den igen for at prøve igen."}},
    {"nb-NO", LANG_NORWEGIAN, false, {
        L"BOO SAH SAH krever en Direct3D 12-kompatibel enhet.\nSe loggene for diagnostikk fra Hardware Orchestra.",
        L"BOO SAH SAH — Grafikkenheten støttes ikke",
        L"Grafikkavspillingen kunne ikke gjenopprettes.\nLyd og tidslinje er fortsatt tilgjengelige. Lukk appen og start den på nytt for å prøve igjen."}},
    {"fi-FI", LANG_FINNISH, false, {
        L"BOO SAH SAH edellyttää Direct3D 12 -yhteensopivaa laitetta.\nHardware Orchestran diagnostiikka löytyy lokeista.",
        L"BOO SAH SAH — Näytönohjainta ei tueta",
        L"Grafiikan toistoa ei voitu palauttaa.\nÄäni ja aikajana ovat edelleen käytettävissä. Sulje sovellus ja käynnistä se uudelleen yrittääksesi uudelleen."}},
};

constexpr size_t kLanguageCount = sizeof(kLanguages) / sizeof(kLanguages[0]);

const LanguageTable& tableFor(uint16_t langId) {
    const uint16_t primary = PRIMARYLANGID(langId);
    const uint16_t sub = SUBLANGID(langId);
    const bool traditional = primary == LANG_CHINESE &&
        (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU);
    for (const LanguageTable& t : kLanguages) {
        if (t.primaryLang == primary && t.traditionalChinese == traditional) return t;
    }
    return kLanguages[0];
}

} // namespace

const wchar_t* uiTextForLanguage(UiText id, uint16_t langId) {
    if (id >= UiText::Count) return L"";
    return tableFor(langId).text[static_cast<size_t>(id)];
}

bool uiLanguageIsRightToLeft(uint16_t langId) {
    return tableFor(langId).primaryLang == LANG_ARABIC;
}

const wchar_t* uiText(UiText id) { return uiTextForLanguage(id, GetUserDefaultUILanguage()); }

bool uiIsRightToLeft() { return uiLanguageIsRightToLeft(GetUserDefaultUILanguage()); }

size_t uiLanguageCount() { return kLanguageCount; }

const char* uiLanguageTag(size_t index) { return index < kLanguageCount ? kLanguages[index].tag : ""; }

const wchar_t* uiTextAt(size_t index, UiText id) {
    if (index >= kLanguageCount || id >= UiText::Count) return L"";
    return kLanguages[index].text[static_cast<size_t>(id)];
}

} // namespace boo
