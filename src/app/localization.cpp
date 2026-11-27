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
    const wchar_t* text[static_cast<size_t>(UiText::Count)]; // In the order of UiText
};

// "Hardware Orchestra", "Direct3D 12" and the title stay as they are in every language.
// English must stay first: it is the fallback.
const LanguageTable kLanguages[] = {
    {"en-US", LANG_ENGLISH, false, {
        L"BOO SAH SAH requires a compatible Direct3D 12 device.\nSee logs for Hardware Orchestra diagnostics.",
        L"BOO SAH SAH — Unsupported Graphics Device",
        L"Graphics playback could not be restored.\nThe audio and timeline remain available. Close the show and restart to retry.",
        L"Loading…",
        L"BOO SAH SAH is an independent production.",
        L"Starting the graphics device",
        L"Reading files",
        L"Compiling shaders",
        L"Preparing the scene",
        L"First start: your graphics driver is preparing shaders for this PC. Later starts are faster.",
        L"BOO SAH SAH — This graphics system could not draw the full show",
        L"BOO SAH SAH could not draw its scenes with this PC's graphics system, so it is showing a simple test picture. Please report it so we can check whether your graphics system can be supported.\n\nOpen the report page now?"}},
    {"fr-FR", LANG_FRENCH, false, {
        L"BOO SAH SAH nécessite un périphérique compatible Direct3D 12.\nConsultez les journaux pour les diagnostics Hardware Orchestra.",
        L"BOO SAH SAH — Périphérique graphique non pris en charge",
        L"La lecture graphique n'a pas pu être rétablie.\nL'audio et la chronologie restent disponibles. Fermez l'application et relancez-la pour réessayer.",
        L"Chargement…",
        L"BOO SAH SAH est une production indépendante.",
        L"Démarrage du périphérique graphique",
        L"Lecture des fichiers",
        L"Compilation des shaders",
        L"Préparation de la scène",
        L"Premier démarrage : le pilote graphique prépare les shaders pour ce PC. Les démarrages suivants sont plus rapides.",
        L"BOO SAH SAH — Ce système graphique n'a pas pu afficher le spectacle complet",
        L"BOO SAH SAH n'a pas pu dessiner ses scènes avec le système graphique de ce PC et affiche donc une simple image de test. Veuillez le signaler afin que nous vérifiions si votre système graphique peut être pris en charge.\n\nOuvrir la page de signalement maintenant ?"}},
    {"de-DE", LANG_GERMAN, false, {
        L"BOO SAH SAH benötigt ein Direct3D 12-kompatibles Gerät.\nDiagnosedaten zu Hardware Orchestra finden Sie in den Protokollen.",
        L"BOO SAH SAH — Grafikgerät wird nicht unterstützt",
        L"Die Grafikwiedergabe konnte nicht wiederhergestellt werden.\nAudio und Zeitleiste bleiben verfügbar. Schließen Sie die App und starten Sie sie neu, um es erneut zu versuchen.",
        L"Wird geladen…",
        L"BOO SAH SAH ist eine unabhängige Produktion.",
        L"Grafikgerät wird gestartet",
        L"Dateien werden gelesen",
        L"Shader werden kompiliert",
        L"Szene wird vorbereitet",
        L"Erster Start: Der Grafiktreiber bereitet die Shader für diesen PC vor. Spätere Starts sind schneller.",
        L"BOO SAH SAH — Dieses Grafiksystem konnte die Show nicht vollständig darstellen",
        L"BOO SAH SAH konnte seine Szenen mit dem Grafiksystem dieses PCs nicht darstellen und zeigt deshalb ein einfaches Testbild. Bitte melden Sie das, damit wir prüfen können, ob Ihr Grafiksystem unterstützt werden kann.\n\nJetzt die Meldeseite öffnen?"}},
    {"es-ES", LANG_SPANISH, false, {
        L"BOO SAH SAH requiere un dispositivo compatible con Direct3D 12.\nConsulta los registros para ver el diagnóstico de Hardware Orchestra.",
        L"BOO SAH SAH — Dispositivo gráfico no compatible",
        L"No se pudo restaurar la reproducción de gráficos.\nEl audio y la línea de tiempo siguen disponibles. Cierra la aplicación y vuelve a iniciarla para reintentarlo.",
        L"Cargando…",
        L"BOO SAH SAH es una producción independiente.",
        L"Iniciando el dispositivo gráfico",
        L"Leyendo archivos",
        L"Compilando sombreadores",
        L"Preparando la escena",
        L"Primer inicio: el controlador gráfico está preparando los sombreadores para este PC. Los siguientes inicios son más rápidos.",
        L"BOO SAH SAH — Este sistema gráfico no pudo mostrar el espectáculo completo",
        L"BOO SAH SAH no pudo dibujar sus escenas con el sistema gráfico de este PC, por lo que muestra una imagen de prueba sencilla. Informa del problema para que podamos comprobar si tu sistema gráfico puede ser compatible.\n\n¿Abrir ahora la página de informes?"}},
    {"it-IT", LANG_ITALIAN, false, {
        L"BOO SAH SAH richiede un dispositivo compatibile con Direct3D 12.\nConsulta i registri per la diagnostica di Hardware Orchestra.",
        L"BOO SAH SAH — Dispositivo grafico non supportato",
        L"Impossibile ripristinare la riproduzione grafica.\nL'audio e la timeline restano disponibili. Chiudi l'app e riavviala per riprovare.",
        L"Caricamento…",
        L"BOO SAH SAH è una produzione indipendente.",
        L"Avvio del dispositivo grafico",
        L"Lettura dei file",
        L"Compilazione degli shader",
        L"Preparazione della scena",
        L"Primo avvio: il driver grafico sta preparando gli shader per questo PC. Gli avvii successivi sono più rapidi.",
        L"BOO SAH SAH — Questo sistema grafico non è riuscito a mostrare lo spettacolo completo",
        L"BOO SAH SAH non è riuscito a disegnare le scene con il sistema grafico di questo PC, quindi mostra una semplice immagine di prova. Segnalacelo, così potremo verificare se il tuo sistema grafico può essere supportato.\n\nAprire ora la pagina per le segnalazioni?"}},
    {"pt-BR", LANG_PORTUGUESE, false, {
        L"O BOO SAH SAH requer um dispositivo compatível com Direct3D 12.\nConsulte os logs para ver o diagnóstico do Hardware Orchestra.",
        L"BOO SAH SAH — Dispositivo gráfico não compatível",
        L"Não foi possível restaurar a reprodução gráfica.\nO áudio e a linha do tempo continuam disponíveis. Feche o aplicativo e reinicie para tentar novamente.",
        L"Carregando…",
        L"BOO SAH SAH é uma produção independente.",
        L"Iniciando o dispositivo gráfico",
        L"Lendo arquivos",
        L"Compilando shaders",
        L"Preparando a cena",
        L"Primeira inicialização: o driver gráfico está preparando os shaders para este PC. As próximas serão mais rápidas.",
        L"BOO SAH SAH — Este sistema gráfico não conseguiu exibir o show completo",
        L"O BOO SAH SAH não conseguiu desenhar as cenas com o sistema gráfico deste PC e está mostrando uma imagem de teste simples. Informe o problema para verificarmos se o seu sistema gráfico pode ser compatível.\n\nAbrir a página de relatos agora?"}},
    {"ja-JP", LANG_JAPANESE, false, {
        L"BOO SAH SAH には Direct3D 12 対応デバイスが必要です。\nHardware Orchestra の診断情報はログを確認してください。",
        L"BOO SAH SAH — サポートされていないグラフィックス デバイス",
        L"グラフィックスの再生を復元できませんでした。\nオーディオとタイムラインは引き続き利用できます。アプリを閉じて再起動し、もう一度お試しください。",
        L"読み込み中…",
        L"BOO SAH SAH は自主制作の作品です。",
        L"グラフィックス デバイスを起動しています",
        L"ファイルを読み込んでいます",
        L"シェーダーをコンパイルしています",
        L"シーンを準備しています",
        L"初回起動: グラフィックス ドライバーがこの PC 用のシェーダーを準備しています。次回からは速くなります。",
        L"BOO SAH SAH — このグラフィックス システムでは本編を表示できませんでした",
        L"BOO SAH SAH はこの PC のグラフィックス システムでシーンを描画できなかったため、簡易テスト画像を表示しています。お使いのグラフィックス システムに対応できるか確認しますので、ご報告ください。\n\n今すぐ報告ページを開きますか？"}},
    {"ko-KR", LANG_KOREAN, false, {
        L"BOO SAH SAH를 실행하려면 Direct3D 12 호환 장치가 필요합니다.\nHardware Orchestra 진단 정보는 로그를 확인하세요.",
        L"BOO SAH SAH — 지원되지 않는 그래픽 장치",
        L"그래픽 재생을 복원할 수 없습니다.\n오디오와 타임라인은 계속 사용할 수 있습니다. 앱을 닫고 다시 시작하여 다시 시도하세요.",
        L"로드 중…",
        L"BOO SAH SAH는 독립 제작 작품입니다.",
        L"그래픽 장치를 시작하는 중",
        L"파일을 읽는 중",
        L"셰이더를 컴파일하는 중",
        L"장면을 준비하는 중",
        L"첫 실행: 그래픽 드라이버가 이 PC용 셰이더를 준비하고 있습니다. 다음부터는 더 빨라집니다.",
        L"BOO SAH SAH — 이 그래픽 시스템에서는 전체 화면을 표시할 수 없습니다",
        L"BOO SAH SAH가 이 PC의 그래픽 시스템으로 장면을 그리지 못해 간단한 테스트 화면을 표시하고 있습니다. 사용 중인 그래픽 시스템을 지원할 수 있는지 확인할 수 있도록 알려 주세요.\n\n지금 신고 페이지를 여시겠습니까?"}},
    {"zh-CN", LANG_CHINESE, false, {
        L"BOO SAH SAH 需要兼容 Direct3D 12 的设备。\n请查看日志中的 Hardware Orchestra 诊断信息。",
        L"BOO SAH SAH — 不支持的图形设备",
        L"无法恢复图形播放。\n音频和时间线仍然可用。请关闭应用并重新启动以重试。",
        L"正在加载…",
        L"《BOO SAH SAH》是一部独立制作的作品。",
        L"正在启动图形设备",
        L"正在读取文件",
        L"正在编译着色器",
        L"正在准备场景",
        L"首次启动：显卡驱动程序正在为这台电脑准备着色器。以后启动会更快。",
        L"BOO SAH SAH — 此图形系统无法显示完整内容",
        L"BOO SAH SAH 无法使用这台电脑的图形系统绘制场景，因此显示的是简单的测试画面。请反馈此问题，以便我们确认能否支持你的图形系统。\n\n现在打开反馈页面吗？"}},
    {"zh-TW", LANG_CHINESE, true, {
        L"BOO SAH SAH 需要相容 Direct3D 12 的裝置。\n請查看記錄檔中的 Hardware Orchestra 診斷資訊。",
        L"BOO SAH SAH — 不支援的圖形裝置",
        L"無法還原圖形播放。\n音訊和時間軸仍可使用。請關閉應用程式並重新啟動以重試。",
        L"載入中…",
        L"《BOO SAH SAH》是一部獨立製作的作品。",
        L"正在啟動圖形裝置",
        L"正在讀取檔案",
        L"正在編譯著色器",
        L"正在準備場景",
        L"首次啟動：顯示卡驅動程式正在為這部電腦準備著色器。之後啟動會更快。",
        L"BOO SAH SAH — 此圖形系統無法顯示完整內容",
        L"BOO SAH SAH 無法使用這部電腦的圖形系統繪製場景，因此目前顯示的是簡單的測試畫面。請回報此問題，讓我們確認能否支援你的圖形系統。\n\n現在開啟回報頁面嗎？"}},
    {"ru-RU", LANG_RUSSIAN, false, {
        L"Для BOO SAH SAH требуется устройство с поддержкой Direct3D 12.\nДиагностику Hardware Orchestra смотрите в журналах.",
        L"BOO SAH SAH — Графическое устройство не поддерживается",
        L"Не удалось восстановить воспроизведение графики.\nЗвук и временная шкала по-прежнему доступны. Закройте приложение и запустите его снова, чтобы повторить попытку.",
        L"Загрузка…",
        L"BOO SAH SAH — независимый проект.",
        L"Запуск графического устройства",
        L"Чтение файлов",
        L"Компиляция шейдеров",
        L"Подготовка сцены",
        L"Первый запуск: графический драйвер готовит шейдеры для этого компьютера. Следующие запуски будут быстрее.",
        L"BOO SAH SAH — эта графическая система не смогла показать всё представление",
        L"BOO SAH SAH не смог отрисовать сцены на графической системе этого компьютера и показывает простую тестовую картинку. Пожалуйста, сообщите об этом, чтобы мы проверили, можно ли поддержать вашу графическую систему.\n\nОткрыть страницу для сообщений сейчас?"}},
    {"pl-PL", LANG_POLISH, false, {
        L"BOO SAH SAH wymaga urządzenia zgodnego z Direct3D 12.\nDiagnostykę Hardware Orchestra znajdziesz w dziennikach.",
        L"BOO SAH SAH — Nieobsługiwane urządzenie graficzne",
        L"Nie udało się przywrócić odtwarzania grafiki.\nDźwięk i oś czasu są nadal dostępne. Zamknij aplikację i uruchom ją ponownie, aby spróbować jeszcze raz.",
        L"Ładowanie…",
        L"BOO SAH SAH to niezależna produkcja.",
        L"Uruchamianie urządzenia graficznego",
        L"Odczytywanie plików",
        L"Kompilowanie shaderów",
        L"Przygotowywanie sceny",
        L"Pierwsze uruchomienie: sterownik graficzny przygotowuje shadery dla tego komputera. Kolejne uruchomienia będą szybsze.",
        L"BOO SAH SAH — ten układ graficzny nie mógł wyświetlić całego pokazu",
        L"BOO SAH SAH nie mógł narysować scen na układzie graficznym tego komputera, więc pokazuje prosty obraz testowy. Zgłoś ten problem, abyśmy mogli sprawdzić, czy Twój układ graficzny da się obsłużyć.\n\nOtworzyć teraz stronę zgłoszeń?"}},
    {"nl-NL", LANG_DUTCH, false, {
        L"BOO SAH SAH vereist een apparaat dat compatibel is met Direct3D 12.\nRaadpleeg de logboeken voor de diagnose van Hardware Orchestra.",
        L"BOO SAH SAH — Grafisch apparaat niet ondersteund",
        L"Het afspelen van beelden kon niet worden hersteld.\nAudio en tijdlijn blijven beschikbaar. Sluit de app en start deze opnieuw om het nogmaals te proberen.",
        L"Laden…",
        L"BOO SAH SAH is een onafhankelijke productie.",
        L"Grafisch apparaat starten",
        L"Bestanden lezen",
        L"Shaders compileren",
        L"Scène voorbereiden",
        L"Eerste start: het grafische stuurprogramma bereidt de shaders voor deze pc voor. Volgende keren gaat het sneller.",
        L"BOO SAH SAH — dit grafische systeem kon niet de volledige show tonen",
        L"BOO SAH SAH kon zijn scènes niet tekenen met het grafische systeem van deze pc en toont daarom een eenvoudig testbeeld. Meld dit, zodat we kunnen nagaan of uw grafische systeem ondersteund kan worden.\n\nDe meldpagina nu openen?"}},
    {"tr-TR", LANG_TURKISH, false, {
        L"BOO SAH SAH, Direct3D 12 uyumlu bir cihaz gerektirir.\nHardware Orchestra tanılama bilgileri için günlüklere bakın.",
        L"BOO SAH SAH — Desteklenmeyen Grafik Cihazı",
        L"Grafik oynatma geri yüklenemedi.\nSes ve zaman çizelgesi kullanılabilir durumda. Yeniden denemek için uygulamayı kapatıp yeniden başlatın.",
        L"Yükleniyor…",
        L"BOO SAH SAH bağımsız bir yapımdır.",
        L"Grafik cihazı başlatılıyor",
        L"Dosyalar okunuyor",
        L"Gölgelendiriciler derleniyor",
        L"Sahne hazırlanıyor",
        L"İlk açılış: grafik sürücüsü bu bilgisayar için gölgelendiricileri hazırlıyor. Sonraki açılışlar daha hızlıdır.",
        L"BOO SAH SAH — bu grafik sistemi gösterinin tamamını görüntüleyemedi",
        L"BOO SAH SAH bu bilgisayarın grafik sistemiyle sahnelerini çizemedi, bu yüzden basit bir test görüntüsü gösteriyor. Grafik sisteminizin desteklenip desteklenemeyeceğini kontrol edebilmemiz için lütfen bildirin.\n\nBildirim sayfası şimdi açılsın mı?"}},
    {"sv-SE", LANG_SWEDISH, false, {
        L"BOO SAH SAH kräver en Direct3D 12-kompatibel enhet.\nSe loggarna för diagnostik från Hardware Orchestra.",
        L"BOO SAH SAH — Grafikenheten stöds inte",
        L"Grafikuppspelningen kunde inte återställas.\nLjudet och tidslinjen är fortfarande tillgängliga. Stäng appen och starta om den för att försöka igen.",
        L"Läser in…",
        L"BOO SAH SAH är en oberoende produktion.",
        L"Startar grafikenheten",
        L"Läser filer",
        L"Kompilerar shaders",
        L"Förbereder scenen",
        L"Första starten: grafikdrivrutinen förbereder shaders för den här datorn. Senare starter går snabbare.",
        L"BOO SAH SAH — det här grafiksystemet kunde inte visa hela showen",
        L"BOO SAH SAH kunde inte rita sina scener med den här datorns grafiksystem och visar därför en enkel testbild. Rapportera det gärna så att vi kan kontrollera om ditt grafiksystem kan stödjas.\n\nÖppna rapportsidan nu?"}},
    {"ar-SA", LANG_ARABIC, false, {
        L"يتطلب BOO SAH SAH جهازًا متوافقًا مع Direct3D 12.\nراجع السجلات للحصول على تشخيصات Hardware Orchestra.",
        L"BOO SAH SAH — جهاز رسومات غير مدعوم",
        L"تعذّر استعادة تشغيل الرسومات.\nلا يزال الصوت والمخطط الزمني متاحين. أغلق التطبيق وأعد تشغيله للمحاولة مرة أخرى.",
        L"جارٍ التحميل…",
        L"BOO SAH SAH عمل من إنتاج مستقل.",
        L"جارٍ تشغيل جهاز الرسومات",
        L"جارٍ قراءة الملفات",
        L"جارٍ تجميع برامج التظليل",
        L"جارٍ تجهيز المشهد",
        L"التشغيل الأول: يجهّز برنامج تشغيل الرسومات برامج التظليل لهذا الكمبيوتر. ستكون مرات التشغيل التالية أسرع.",
        L"BOO SAH SAH — تعذّر على نظام الرسومات هذا عرض العرض الكامل",
        L"تعذّر على BOO SAH SAH رسم مشاهده باستخدام نظام الرسومات في هذا الكمبيوتر، لذلك يعرض صورة اختبار بسيطة. يُرجى الإبلاغ عن ذلك لنتحقق مما إذا كان يمكن دعم نظام الرسومات لديك.\n\nهل تريد فتح صفحة الإبلاغ الآن؟"}},
    {"hi-IN", LANG_HINDI, false, {
        L"BOO SAH SAH के लिए Direct3D 12 संगत डिवाइस आवश्यक है।\nHardware Orchestra डायग्नोस्टिक्स के लिए लॉग देखें।",
        L"BOO SAH SAH — असमर्थित ग्राफ़िक्स डिवाइस",
        L"ग्राफ़िक्स प्लेबैक पुनर्स्थापित नहीं किया जा सका।\nऑडियो और टाइमलाइन अब भी उपलब्ध हैं। फिर से प्रयास करने के लिए ऐप बंद करके दोबारा शुरू करें।",
        L"लोड हो रहा है…",
        L"BOO SAH SAH का निर्माण स्वतंत्र रूप से किया गया है।",
        L"ग्राफ़िक्स डिवाइस शुरू हो रहा है",
        L"फ़ाइलें पढ़ी जा रही हैं",
        L"शेडर कंपाइल हो रहे हैं",
        L"दृश्य तैयार हो रहा है",
        L"पहली शुरुआत: ग्राफ़िक्स ड्राइवर इस PC के लिए शेडर तैयार कर रहा है। अगली बार शुरुआत तेज़ होगी।",
        L"BOO SAH SAH — यह ग्राफ़िक्स सिस्टम पूरा शो नहीं दिखा सका",
        L"BOO SAH SAH इस PC के ग्राफ़िक्स सिस्टम से अपने दृश्य नहीं बना सका, इसलिए एक साधारण परीक्षण चित्र दिखा रहा है। कृपया इसकी रिपोर्ट करें, ताकि हम जाँच सकें कि आपका ग्राफ़िक्स सिस्टम समर्थित हो सकता है या नहीं।\n\nक्या अभी रिपोर्ट पेज खोलें?"}},
    {"th-TH", LANG_THAI, false, {
        L"BOO SAH SAH ต้องใช้อุปกรณ์ที่รองรับ Direct3D 12\nดูข้อมูลการวินิจฉัยของ Hardware Orchestra ได้ในบันทึก",
        L"BOO SAH SAH — ไม่รองรับอุปกรณ์กราฟิกนี้",
        L"ไม่สามารถกู้คืนการเล่นกราฟิกได้\nเสียงและไทม์ไลน์ยังคงใช้งานได้ ปิดแอปแล้วเปิดใหม่เพื่อลองอีกครั้ง",
        L"กำลังโหลด…",
        L"BOO SAH SAH เป็นผลงานสร้างสรรค์อิสระ",
        L"กำลังเริ่มอุปกรณ์กราฟิก",
        L"กำลังอ่านไฟล์",
        L"กำลังคอมไพล์เชเดอร์",
        L"กำลังเตรียมฉาก",
        L"การเปิดครั้งแรก: ไดรเวอร์กราฟิกกำลังเตรียมเชเดอร์สำหรับพีซีเครื่องนี้ ครั้งต่อไปจะเร็วขึ้น",
        L"BOO SAH SAH — ระบบกราฟิกนี้ไม่สามารถแสดงโชว์ได้ครบถ้วน",
        L"BOO SAH SAH ไม่สามารถวาดฉากด้วยระบบกราฟิกของพีซีเครื่องนี้ จึงแสดงภาพทดสอบแบบง่ายแทน โปรดรายงานปัญหานี้ เพื่อให้เราตรวจสอบว่าสามารถรองรับระบบกราฟิกของคุณได้หรือไม่\n\nเปิดหน้ารายงานปัญหาเลยหรือไม่"}},
    {"vi-VN", LANG_VIETNAMESE, false, {
        L"BOO SAH SAH yêu cầu thiết bị tương thích với Direct3D 12.\nXem nhật ký để biết thông tin chẩn đoán của Hardware Orchestra.",
        L"BOO SAH SAH — Thiết bị đồ họa không được hỗ trợ",
        L"Không thể khôi phục việc phát đồ họa.\nÂm thanh và dòng thời gian vẫn khả dụng. Hãy đóng ứng dụng và khởi động lại để thử lại.",
        L"Đang tải…",
        L"BOO SAH SAH là một tác phẩm được sản xuất độc lập.",
        L"Đang khởi động thiết bị đồ họa",
        L"Đang đọc tệp",
        L"Đang biên dịch shader",
        L"Đang chuẩn bị cảnh",
        L"Lần khởi động đầu tiên: trình điều khiển đồ họa đang chuẩn bị shader cho PC này. Các lần sau sẽ nhanh hơn.",
        L"BOO SAH SAH — Hệ thống đồ họa này không thể hiển thị đầy đủ chương trình",
        L"BOO SAH SAH không thể vẽ các cảnh bằng hệ thống đồ họa của PC này nên đang hiển thị một hình thử nghiệm đơn giản. Vui lòng báo cáo để chúng tôi kiểm tra xem có thể hỗ trợ hệ thống đồ họa của bạn hay không.\n\nMở trang báo cáo ngay?"}},
    {"id-ID", LANG_INDONESIAN, false, {
        L"BOO SAH SAH memerlukan perangkat yang kompatibel dengan Direct3D 12.\nLihat log untuk diagnostik Hardware Orchestra.",
        L"BOO SAH SAH — Perangkat Grafis Tidak Didukung",
        L"Pemutaran grafis tidak dapat dipulihkan.\nAudio dan lini masa tetap tersedia. Tutup aplikasi lalu mulai ulang untuk mencoba lagi.",
        L"Memuat…",
        L"BOO SAH SAH adalah karya produksi independen.",
        L"Memulai perangkat grafis",
        L"Membaca berkas",
        L"Mengompilasi shader",
        L"Menyiapkan adegan",
        L"Pertama kali dijalankan: driver grafis sedang menyiapkan shader untuk PC ini. Berikutnya akan lebih cepat.",
        L"BOO SAH SAH — Sistem grafis ini tidak dapat menampilkan pertunjukan lengkap",
        L"BOO SAH SAH tidak dapat menggambar adegannya dengan sistem grafis PC ini, sehingga menampilkan gambar uji sederhana. Harap laporkan agar kami dapat memeriksa apakah sistem grafis Anda bisa didukung.\n\nBuka halaman laporan sekarang?"}},
    {"cs-CZ", LANG_CZECH, false, {
        L"BOO SAH SAH vyžaduje zařízení kompatibilní s Direct3D 12.\nDiagnostiku Hardware Orchestra najdete v protokolech.",
        L"BOO SAH SAH — Nepodporované grafické zařízení",
        L"Přehrávání grafiky se nepodařilo obnovit.\nZvuk a časová osa zůstávají k dispozici. Zavřete aplikaci a spusťte ji znovu.",
        L"Načítání…",
        L"BOO SAH SAH je nezávislá produkce.",
        L"Spouštění grafického zařízení",
        L"Načítání souborů",
        L"Kompilace shaderů",
        L"Příprava scény",
        L"První spuštění: grafický ovladač připravuje shadery pro tento počítač. Další spuštění budou rychlejší.",
        L"BOO SAH SAH — Tento grafický systém nedokázal zobrazit celou show",
        L"BOO SAH SAH nedokázal na grafickém systému tohoto počítače vykreslit své scény, a proto zobrazuje jednoduchý testovací obraz. Nahlaste to prosím, abychom mohli ověřit, zda lze váš grafický systém podporovat.\n\nOtevřít teď stránku pro hlášení?"}},
    {"uk-UA", LANG_UKRAINIAN, false, {
        L"Для BOO SAH SAH потрібен пристрій із підтримкою Direct3D 12.\nДіагностику Hardware Orchestra дивіться в журналах.",
        L"BOO SAH SAH — Графічний пристрій не підтримується",
        L"Не вдалося відновити відтворення графіки.\nЗвук і часова шкала залишаються доступними. Закрийте застосунок і запустіть його знову, щоб повторити спробу.",
        L"Завантаження…",
        L"BOO SAH SAH — незалежний проєкт.",
        L"Запуск графічного пристрою",
        L"Читання файлів",
        L"Компіляція шейдерів",
        L"Підготовка сцени",
        L"Перший запуск: графічний драйвер готує шейдери для цього комп’ютера. Наступні запуски будуть швидшими.",
        L"BOO SAH SAH — ця графічна система не змогла показати все шоу",
        L"BOO SAH SAH не зміг намалювати сцени на графічній системі цього комп’ютера, тому показує просте тестове зображення. Повідомте про це, щоб ми перевірили, чи можна підтримати вашу графічну систему.\n\nВідкрити сторінку повідомлень зараз?"}},
    {"da-DK", LANG_DANISH, false, {
        L"BOO SAH SAH kræver en Direct3D 12-kompatibel enhed.\nSe logfilerne for diagnosticering fra Hardware Orchestra.",
        L"BOO SAH SAH — Grafikenheden understøttes ikke",
        L"Grafikafspilningen kunne ikke gendannes.\nLyd og tidslinje er stadig tilgængelige. Luk appen, og start den igen for at prøve igen.",
        L"Indlæser…",
        L"BOO SAH SAH er en uafhængig produktion.",
        L"Starter grafikenheden",
        L"Læser filer",
        L"Kompilerer shaders",
        L"Forbereder scenen",
        L"Første start: Grafikdriveren forbereder shaders til denne pc. Senere starter går hurtigere.",
        L"BOO SAH SAH — dette grafiksystem kunne ikke vise hele showet",
        L"BOO SAH SAH kunne ikke tegne sine scener med denne pc’s grafiksystem og viser derfor et simpelt testbillede. Rapporter det, så vi kan undersøge, om dit grafiksystem kan understøttes.\n\nVil du åbne rapportsiden nu?"}},
    {"nb-NO", LANG_NORWEGIAN, false, {
        L"BOO SAH SAH krever en Direct3D 12-kompatibel enhet.\nSe loggene for diagnostikk fra Hardware Orchestra.",
        L"BOO SAH SAH — Grafikkenheten støttes ikke",
        L"Grafikkavspillingen kunne ikke gjenopprettes.\nLyd og tidslinje er fortsatt tilgjengelige. Lukk appen og start den på nytt for å prøve igjen.",
        L"Laster inn…",
        L"BOO SAH SAH er en uavhengig produksjon.",
        L"Starter grafikkenheten",
        L"Leser filer",
        L"Kompilerer shadere",
        L"Klargjør scenen",
        L"Første oppstart: Grafikkdriveren klargjør shadere for denne PC-en. Senere oppstarter går raskere.",
        L"BOO SAH SAH — dette grafikksystemet kunne ikke vise hele showet",
        L"BOO SAH SAH kunne ikke tegne scenene sine med grafikksystemet i denne PC-en og viser derfor et enkelt testbilde. Rapporter det, slik at vi kan sjekke om grafikksystemet ditt kan støttes.\n\nVil du åpne rapportsiden nå?"}},
    {"fi-FI", LANG_FINNISH, false, {
        L"BOO SAH SAH edellyttää Direct3D 12 -yhteensopivaa laitetta.\nHardware Orchestran diagnostiikka löytyy lokeista.",
        L"BOO SAH SAH — Näytönohjainta ei tueta",
        L"Grafiikan toistoa ei voitu palauttaa.\nÄäni ja aikajana ovat edelleen käytettävissä. Sulje sovellus ja käynnistä se uudelleen yrittääksesi uudelleen.",
        L"Ladataan…",
        L"BOO SAH SAH on itsenäinen tuotanto.",
        L"Käynnistetään näytönohjainta",
        L"Luetaan tiedostoja",
        L"Käännetään varjostimia",
        L"Valmistellaan näkymää",
        L"Ensimmäinen käynnistys: näytönohjaimen ajuri valmistelee varjostimet tälle tietokoneelle. Seuraavat käynnistykset ovat nopeampia.",
        L"BOO SAH SAH — tämä grafiikkajärjestelmä ei pystynyt näyttämään koko esitystä",
        L"BOO SAH SAH ei pystynyt piirtämään kohtauksiaan tämän tietokoneen grafiikkajärjestelmällä, joten se näyttää yksinkertaisen testikuvan. Ilmoita asiasta, jotta voimme tarkistaa, voidaanko grafiikkajärjestelmääsi tukea.\n\nAvataanko ilmoitussivu nyt?"}},
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
