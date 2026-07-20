#include "localization.hpp"

#include <algorithm>
#include <unordered_map>

namespace opennow
{
namespace
{

using Dictionary = std::unordered_map<std::string, std::string>;

std::string g_language = "en";

const Dictionary& DictionaryFor(const std::string& code)
{
    static const Dictionary empty;
    static const std::unordered_map<std::string, Dictionary> dictionaries = {
        {"ru", {
            {"Store", "Магазин"}, {"Library", "Библиотека"}, {"Settings", "Настройки"},
            {"My Library", "Моя библиотека"}, {"Search", "Поиск"}, {"Close", "Закрыть"},
            {"Cancel", "Отмена"}, {"Account", "Аккаунт"}, {"Stream", "Поток"},
            {"Game", "Игра"}, {"Controls", "Управление"}, {"Audio", "Аудио"},
            {"Storage", "Хранилище"}, {"Interface", "Интерфейс"},
            {"Language", "Язык интерфейса"}, {"Choose the launcher language.", "Выберите язык интерфейса приложения."},
            {"CATEGORIES", "КАТЕГОРИИ"}, {"All changes saved", "Все изменения сохранены"},
            {"Unsaved changes  |  X Save", "Есть изменения  |  X Сохранить"},
            {"Settings are already up to date", "Настройки уже актуальны"},
            {"Settings saved; changes apply to the next stream", "Настройки сохранены; параметры потока применятся к следующему сеансу"},
            {"Unsaved changes reverted", "Несохраненные изменения отменены"},
            {"Quality profile", "Профиль качества"}, {"Preset", "Профиль"},
            {"Resolution", "Разрешение"}, {"Frame rate", "Частота кадров"},
            {"Maximum bitrate", "Максимальный битрейт"}, {"Motion clarity", "Четкость в движении"},
            {"Video backend", "Видеодекодер"}, {"Decoder & delivery", "Декодирование и доставка"},
            {"Recovery", "Восстановление"}, {"Diagnostics", "Диагностика"},
            {"Debug diagnostics", "Отладочная диагностика"}, {"Enabled", "Включено"},
            {"Disabled", "Выключено"}, {"Muted", "Без звука"},
            {"Stream audio", "Звук трансляции"}, {"Audio output", "Вывод звука"},
            {"Volume boost", "Усиление громкости"}, {"Playback buffer", "Буфер воспроизведения"},
            {"Output format", "Формат вывода"}, {"Game preferences", "Настройки игры"},
            {"Game language", "Язык игры"}, {"Save in-game graphics settings", "Сохранять настройки графики игры"},
            {"Controller layout", "Раскладка контроллера"}, {"Face buttons", "Кнопки ABXY"},
            {"Connected account", "Подключенный аккаунт"}, {"Status", "Статус"},
            {"Not connected", "Не подключен"}, {"User", "Пользователь"},
            {"Membership", "Подписка"}, {"Provider", "Провайдер"},
            {"Authentication", "Авторизация"}, {"Quick sign-in", "Быстрый вход"},
            {"Saved accounts", "Сохраненные аккаунты"}, {"Session", "Сессия"},
            {"Session details", "Данные сессии"}, {"Refresh authorization", "Обновить авторизацию"},
            {"Choose saved account", "Выбрать сохраненный аккаунт"}, {"Forget saved passwords", "Забыть сохраненные пароли"},
            {"Account removal", "Удаление аккаунта"}, {"Remove active account", "Удалить активный аккаунт"},
            {"Remove every account", "Удалить все аккаунты"}, {"View", "Открыть"},
            {"Refresh", "Обновить"}, {"Choose", "Выбрать"}, {"Forget", "Забыть"},
            {"Remove", "Удалить"}, {"Remove all", "Удалить все"}, {"Restore", "Восстановить"},
            {"Cover cache", "Кэш обложек"}, {"Cover files", "Файлы обложек"},
            {"Disk usage", "Использование диска"}, {"Public catalog", "Публичный каталог"},
            {"Owned library", "Моя библиотека"}, {"Loaded", "Загружено"}, {"Not loaded", "Не загружено"},
            {"Inspect shared cache", "Проверить общий кэш"}, {"Clear cover artwork", "Очистить обложки"},
            {"Inspect", "Проверить"}, {"Clear", "Очистить"}, {"System", "Система"},
            {"Open diagnostics", "Открыть диагностику"}, {"Open", "Открыть"},
            {"Sign in to GeForce NOW", "Войти в GeForce NOW"}, {"Refresh library", "Обновить библиотеку"},
            {"Sign out", "Выйти"}, {"Add another account", "Добавить аккаунт"},
            {"Reconnect this account", "Переподключить аккаунт"}, {"No account", "Нет аккаунта"},
            {"Play on GeForce NOW", "Играть в GeForce NOW"}, {"Play from Store", "Играть из магазина"},
            {"In your library", "В вашей библиотеке"}, {"Supported in GeForce NOW", "Поддерживается в GeForce NOW"},
            {"Choose game store", "Выберите магазин игры"}, {"Not Logged In", "Вход не выполнен"},
            {"Launch Error", "Ошибка запуска"}, {"NOW LOADING", "ЗАПУСК"},
            {"Checking your NVIDIA account", "Проверка аккаунта NVIDIA"},
            {"Requesting a cloud rig", "Запрос облачного компьютера"},
            {"Waiting in queue...", "Ожидание в очереди..."}, {"Preparing your cloud rig", "Подготовка облачного компьютера"},
            {"Waiting for an available cloud rig", "Ожидание свободного облачного компьютера"},
            {"Session could not start", "Не удалось запустить сессию"}, {"Cancel session", "Отменить сессию"},
            {"Choose how to sign in", "Выберите способ входа"}, {"Welcome back", "С возвращением"},
            {"Connect your account", "Подключите аккаунт"}, {"NVIDIA account", "Аккаунт NVIDIA"},
            {"No saved account yet", "Сохраненного аккаунта пока нет"}, {"QUICK SIGN-IN READY", "БЫСТРЫЙ ВХОД ГОТОВ"},
            {"NEW ACCOUNT", "НОВЫЙ АККАУНТ"}, {"Use another NVIDIA account", "Использовать другой аккаунт NVIDIA"},
            {"Enter email and password", "Ввести почту и пароль"}, {"Phone / PC fallback for CAPTCHA or passkey", "Вход через телефон / ПК для CAPTCHA или ключа доступа"},
            {"SIGN-IN PREFERENCES", "НАСТРОЙКИ ВХОДА"}, {"Remember sign-in: ON", "Запоминать вход: ВКЛ"},
            {"Cancel active sign-in", "Отменить текущий вход"}, {"Approve the NVIDIA sign-in by email", "Подтвердите вход NVIDIA по электронной почте"},
            {"Cancel sign-in", "Отменить вход"}, {"Signing in directly with NVIDIA...", "Вход напрямую через NVIDIA..."},
            {"Sign-in cancelled", "Вход отменен"}, {"Waiting for NVIDIA verification", "Ожидание подтверждения NVIDIA"},
            {"Login failed", "Ошибка входа"}, {"Checking saved sign-in", "Проверка сохраненного входа"},
            {"SAVED NVIDIA ACCOUNT", "СОХРАНЕННЫЙ АККАУНТ NVIDIA"}
        }},
        {"uk", {
            {"Store", "Магазин"}, {"Library", "Бібліотека"}, {"Settings", "Налаштування"},
            {"My Library", "Моя бібліотека"}, {"Search", "Пошук"}, {"Close", "Закрити"},
            {"Cancel", "Скасувати"}, {"Account", "Обліковий запис"}, {"Stream", "Трансляція"},
            {"Game", "Гра"}, {"Controls", "Керування"}, {"Audio", "Аудіо"},
            {"Storage", "Сховище"}, {"Interface", "Інтерфейс"}, {"Language", "Мова інтерфейсу"},
            {"Choose the launcher language.", "Виберіть мову інтерфейсу застосунку."},
            {"CATEGORIES", "КАТЕГОРІЇ"}, {"All changes saved", "Усі зміни збережено"},
            {"Unsaved changes  |  X Save", "Є зміни  |  X Зберегти"},
            {"Quality profile", "Профіль якості"}, {"Preset", "Профіль"}, {"Resolution", "Роздільна здатність"},
            {"Frame rate", "Частота кадрів"}, {"Maximum bitrate", "Максимальний бітрейт"},
            {"Diagnostics", "Діагностика"}, {"Enabled", "Увімкнено"}, {"Disabled", "Вимкнено"},
            {"Stream audio", "Звук трансляції"}, {"Volume boost", "Підсилення гучності"},
            {"Game preferences", "Налаштування гри"}, {"Game language", "Мова гри"},
            {"Controller layout", "Розкладка контролера"}, {"Face buttons", "Кнопки ABXY"},
            {"Connected account", "Підключений обліковий запис"}, {"Status", "Стан"},
            {"Not connected", "Не підключено"}, {"User", "Користувач"}, {"Membership", "Підписка"},
            {"Authentication", "Авторизація"}, {"Quick sign-in", "Швидкий вхід"},
            {"Saved accounts", "Збережені облікові записи"}, {"Session", "Сеанс"},
            {"Refresh", "Оновити"}, {"Choose", "Вибрати"}, {"Remove", "Видалити"},
            {"Cover cache", "Кеш обкладинок"}, {"System", "Система"},
            {"Sign in to GeForce NOW", "Увійти в GeForce NOW"}, {"Refresh library", "Оновити бібліотеку"},
            {"Sign out", "Вийти"}, {"Play on GeForce NOW", "Грати в GeForce NOW"},
            {"In your library", "У вашій бібліотеці"}, {"NOW LOADING", "ЗАПУСК"},
            {"Checking your NVIDIA account", "Перевірка облікового запису NVIDIA"},
            {"Waiting in queue...", "Очікування в черзі..."}, {"Preparing your cloud rig", "Підготовка хмарного комп'ютера"},
            {"Cancel session", "Скасувати сеанс"}, {"Choose how to sign in", "Виберіть спосіб входу"},
            {"Welcome back", "З поверненням"}, {"Use another NVIDIA account", "Інший обліковий запис NVIDIA"},
            {"Enter email and password", "Ввести пошту та пароль"}, {"Login failed", "Помилка входу"}
        }},
        {"es", {
            {"Store", "Tienda"}, {"Library", "Biblioteca"}, {"Settings", "Ajustes"}, {"My Library", "Mi biblioteca"},
            {"Search", "Buscar"}, {"Close", "Cerrar"}, {"Cancel", "Cancelar"}, {"Account", "Cuenta"},
            {"Stream", "Transmisión"}, {"Game", "Juego"}, {"Controls", "Controles"}, {"Audio", "Audio"},
            {"Storage", "Almacenamiento"}, {"Interface", "Interfaz"}, {"Language", "Idioma de la interfaz"},
            {"Choose the launcher language.", "Elige el idioma de la aplicación."}, {"CATEGORIES", "CATEGORÍAS"},
            {"All changes saved", "Todos los cambios guardados"}, {"Unsaved changes  |  X Save", "Cambios sin guardar  |  X Guardar"},
            {"Quality profile", "Perfil de calidad"}, {"Preset", "Preajuste"}, {"Resolution", "Resolución"},
            {"Frame rate", "Frecuencia de imagen"}, {"Maximum bitrate", "Bitrate máximo"},
            {"Diagnostics", "Diagnóstico"}, {"Enabled", "Activado"}, {"Disabled", "Desactivado"},
            {"Stream audio", "Audio de transmisión"}, {"Volume boost", "Amplificación de volumen"},
            {"Game preferences", "Preferencias del juego"}, {"Game language", "Idioma del juego"},
            {"Controller layout", "Distribución del mando"}, {"Face buttons", "Botones ABXY"},
            {"Connected account", "Cuenta conectada"}, {"Status", "Estado"}, {"Not connected", "Sin conexión"},
            {"Quick sign-in", "Inicio rápido"}, {"Saved accounts", "Cuentas guardadas"},
            {"Refresh", "Actualizar"}, {"Choose", "Elegir"}, {"Remove", "Eliminar"},
            {"Sign in to GeForce NOW", "Iniciar sesión en GeForce NOW"}, {"Refresh library", "Actualizar biblioteca"},
            {"Sign out", "Cerrar sesión"}, {"Play on GeForce NOW", "Jugar en GeForce NOW"},
            {"In your library", "En tu biblioteca"}, {"NOW LOADING", "CARGANDO"},
            {"Checking your NVIDIA account", "Comprobando tu cuenta NVIDIA"}, {"Waiting in queue...", "Esperando en la cola..."},
            {"Preparing your cloud rig", "Preparando tu equipo en la nube"}, {"Cancel session", "Cancelar sesión"},
            {"Choose how to sign in", "Elige cómo iniciar sesión"}, {"Welcome back", "Bienvenido de nuevo"},
            {"Enter email and password", "Introducir correo y contraseña"}, {"Login failed", "Error de inicio de sesión"}
        }},
        {"it", {
            {"Store", "Negozio"}, {"Library", "Libreria"}, {"Settings", "Impostazioni"}, {"My Library", "La mia libreria"},
            {"Search", "Cerca"}, {"Close", "Chiudi"}, {"Cancel", "Annulla"}, {"Account", "Account"},
            {"Stream", "Streaming"}, {"Game", "Gioco"}, {"Controls", "Comandi"}, {"Audio", "Audio"},
            {"Storage", "Archiviazione"}, {"Interface", "Interfaccia"}, {"Language", "Lingua dell'interfaccia"},
            {"Choose the launcher language.", "Scegli la lingua dell'applicazione."}, {"CATEGORIES", "CATEGORIE"},
            {"All changes saved", "Tutte le modifiche sono salvate"}, {"Unsaved changes  |  X Save", "Modifiche non salvate  |  X Salva"},
            {"Quality profile", "Profilo qualità"}, {"Preset", "Profilo"}, {"Resolution", "Risoluzione"},
            {"Frame rate", "Frequenza fotogrammi"}, {"Maximum bitrate", "Bitrate massimo"},
            {"Diagnostics", "Diagnostica"}, {"Enabled", "Attivo"}, {"Disabled", "Disattivato"},
            {"Game language", "Lingua del gioco"}, {"Controller layout", "Layout controller"},
            {"Connected account", "Account collegato"}, {"Quick sign-in", "Accesso rapido"},
            {"Refresh", "Aggiorna"}, {"Choose", "Scegli"}, {"Remove", "Rimuovi"},
            {"Sign in to GeForce NOW", "Accedi a GeForce NOW"}, {"Refresh library", "Aggiorna libreria"},
            {"Sign out", "Esci"}, {"Play on GeForce NOW", "Gioca su GeForce NOW"}, {"In your library", "Nella tua libreria"},
            {"NOW LOADING", "CARICAMENTO"}, {"Checking your NVIDIA account", "Verifica dell'account NVIDIA"},
            {"Waiting in queue...", "In attesa in coda..."}, {"Preparing your cloud rig", "Preparazione del PC cloud"},
            {"Cancel session", "Annulla sessione"}, {"Choose how to sign in", "Scegli come accedere"},
            {"Welcome back", "Bentornato"}, {"Login failed", "Accesso non riuscito"}
        }},
        {"fr", {
            {"Store", "Boutique"}, {"Library", "Bibliothèque"}, {"Settings", "Paramètres"}, {"My Library", "Ma bibliothèque"},
            {"Search", "Rechercher"}, {"Close", "Fermer"}, {"Cancel", "Annuler"}, {"Account", "Compte"},
            {"Stream", "Streaming"}, {"Game", "Jeu"}, {"Controls", "Commandes"}, {"Audio", "Audio"},
            {"Storage", "Stockage"}, {"Interface", "Interface"}, {"Language", "Langue de l'interface"},
            {"Choose the launcher language.", "Choisissez la langue de l'application."}, {"CATEGORIES", "CATÉGORIES"},
            {"All changes saved", "Toutes les modifications sont enregistrées"}, {"Unsaved changes  |  X Save", "Modifications non enregistrées  |  X Enregistrer"},
            {"Quality profile", "Profil de qualité"}, {"Preset", "Préréglage"}, {"Resolution", "Résolution"},
            {"Frame rate", "Fréquence d'images"}, {"Maximum bitrate", "Débit maximal"},
            {"Diagnostics", "Diagnostic"}, {"Enabled", "Activé"}, {"Disabled", "Désactivé"},
            {"Stream audio", "Audio du streaming"}, {"Game language", "Langue du jeu"},
            {"Controller layout", "Disposition de la manette"}, {"Connected account", "Compte connecté"},
            {"Quick sign-in", "Connexion rapide"}, {"Refresh", "Actualiser"}, {"Choose", "Choisir"}, {"Remove", "Supprimer"},
            {"Sign in to GeForce NOW", "Se connecter à GeForce NOW"}, {"Refresh library", "Actualiser la bibliothèque"},
            {"Sign out", "Se déconnecter"}, {"Play on GeForce NOW", "Jouer sur GeForce NOW"}, {"In your library", "Dans votre bibliothèque"},
            {"NOW LOADING", "CHARGEMENT"}, {"Checking your NVIDIA account", "Vérification de votre compte NVIDIA"},
            {"Waiting in queue...", "En attente dans la file..."}, {"Preparing your cloud rig", "Préparation de votre machine cloud"},
            {"Cancel session", "Annuler la session"}, {"Choose how to sign in", "Choisissez comment vous connecter"},
            {"Welcome back", "Bon retour"}, {"Login failed", "Échec de la connexion"}
        }},
        {"pl", {
            {"Store", "Sklep"}, {"Library", "Biblioteka"}, {"Settings", "Ustawienia"}, {"My Library", "Moja biblioteka"},
            {"Search", "Szukaj"}, {"Close", "Zamknij"}, {"Cancel", "Anuluj"}, {"Account", "Konto"},
            {"Stream", "Strumień"}, {"Game", "Gra"}, {"Controls", "Sterowanie"}, {"Audio", "Dźwięk"},
            {"Storage", "Pamięć"}, {"Interface", "Interfejs"}, {"Language", "Język interfejsu"},
            {"Choose the launcher language.", "Wybierz język aplikacji."}, {"CATEGORIES", "KATEGORIE"},
            {"All changes saved", "Wszystkie zmiany zapisano"}, {"Unsaved changes  |  X Save", "Niezapisane zmiany  |  X Zapisz"},
            {"Quality profile", "Profil jakości"}, {"Preset", "Profil"}, {"Resolution", "Rozdzielczość"},
            {"Frame rate", "Liczba klatek"}, {"Maximum bitrate", "Maksymalny bitrate"},
            {"Diagnostics", "Diagnostyka"}, {"Enabled", "Włączone"}, {"Disabled", "Wyłączone"},
            {"Game language", "Język gry"}, {"Controller layout", "Układ kontrolera"},
            {"Connected account", "Połączone konto"}, {"Quick sign-in", "Szybkie logowanie"},
            {"Refresh", "Odśwież"}, {"Choose", "Wybierz"}, {"Remove", "Usuń"},
            {"Sign in to GeForce NOW", "Zaloguj do GeForce NOW"}, {"Refresh library", "Odśwież bibliotekę"},
            {"Sign out", "Wyloguj"}, {"Play on GeForce NOW", "Graj w GeForce NOW"}, {"In your library", "W twojej bibliotece"},
            {"NOW LOADING", "URUCHAMIANIE"}, {"Checking your NVIDIA account", "Sprawdzanie konta NVIDIA"},
            {"Waiting in queue...", "Oczekiwanie w kolejce..."}, {"Preparing your cloud rig", "Przygotowywanie komputera w chmurze"},
            {"Cancel session", "Anuluj sesję"}, {"Choose how to sign in", "Wybierz sposób logowania"},
            {"Welcome back", "Witaj ponownie"}, {"Login failed", "Logowanie nie powiodło się"}
        }},
        {"zh-CN", {
            {"Store", "商店"}, {"Library", "游戏库"}, {"Settings", "设置"}, {"My Library", "我的游戏库"},
            {"Search", "搜索"}, {"Close", "关闭"}, {"Cancel", "取消"}, {"Account", "账户"},
            {"Stream", "串流"}, {"Game", "游戏"}, {"Controls", "控制"}, {"Audio", "音频"},
            {"Storage", "存储"}, {"Interface", "界面"}, {"Language", "界面语言"},
            {"Choose the launcher language.", "选择应用界面语言。"}, {"CATEGORIES", "分类"},
            {"All changes saved", "所有更改已保存"}, {"Unsaved changes  |  X Save", "有未保存的更改  |  X 保存"},
            {"Quality profile", "画质预设"}, {"Preset", "预设"}, {"Resolution", "分辨率"},
            {"Frame rate", "帧率"}, {"Maximum bitrate", "最大码率"}, {"Diagnostics", "诊断"},
            {"Enabled", "已启用"}, {"Disabled", "已禁用"}, {"Stream audio", "串流音频"},
            {"Game language", "游戏语言"}, {"Controller layout", "手柄布局"},
            {"Connected account", "已连接账户"}, {"Quick sign-in", "快速登录"},
            {"Refresh", "刷新"}, {"Choose", "选择"}, {"Remove", "删除"},
            {"Sign in to GeForce NOW", "登录 GeForce NOW"}, {"Refresh library", "刷新游戏库"},
            {"Sign out", "退出登录"}, {"Play on GeForce NOW", "在 GeForce NOW 上游玩"}, {"In your library", "已在游戏库中"},
            {"NOW LOADING", "正在启动"}, {"Checking your NVIDIA account", "正在检查 NVIDIA 账户"},
            {"Waiting in queue...", "正在排队..."}, {"Preparing your cloud rig", "正在准备云端设备"},
            {"Cancel session", "取消会话"}, {"Choose how to sign in", "选择登录方式"},
            {"Welcome back", "欢迎回来"}, {"Login failed", "登录失败"}
        }}
    };

    const auto found = dictionaries.find(code);
    return found == dictionaries.end() ? empty : found->second;
}

} // namespace

const std::vector<InterfaceLanguageOption>& InterfaceLanguageOptions()
{
    static const std::vector<InterfaceLanguageOption> options = {
        {"en", "English"}, {"zh-CN", "简体中文"}, {"es", "Español"},
        {"ru", "Русский"}, {"it", "Italiano"}, {"fr", "Français"},
        {"pl", "Polski"}, {"uk", "Українська"},
    };
    return options;
}

bool IsSupportedInterfaceLanguage(const std::string& code)
{
    const auto& options = InterfaceLanguageOptions();
    return std::any_of(options.begin(), options.end(), [&code](const auto& option) {
        return option.code == code;
    });
}

std::string InterfaceLanguageLabel(const std::string& code)
{
    const auto& options = InterfaceLanguageOptions();
    const auto found = std::find_if(options.begin(), options.end(), [&code](const auto& option) {
        return option.code == code;
    });
    return found == options.end() ? std::string("English") : found->label;
}

void SetInterfaceLanguage(const std::string& code)
{
    g_language = IsSupportedInterfaceLanguage(code) ? code : "en";
}

const std::string& GetInterfaceLanguage()
{
    return g_language;
}

std::string Tr(const std::string& english)
{
    if (g_language == "en" || english.empty())
        return english;
    const auto& dictionary = DictionaryFor(g_language);
    const auto found = dictionary.find(english);
    return found == dictionary.end() ? english : found->second;
}

std::string Tr(const char* english)
{
    return Tr(english ? std::string(english) : std::string {});
}

std::string TrFormat(const std::string& english, const std::vector<std::string>& values)
{
    std::string result = Tr(english);
    for (size_t index = 0; index < values.size(); ++index)
    {
        const std::string marker = "{" + std::to_string(index) + "}";
        size_t position = 0;
        while ((position = result.find(marker, position)) != std::string::npos)
        {
            result.replace(position, marker.size(), values[index]);
            position += values[index].size();
        }
    }
    return result;
}

} // namespace opennow
