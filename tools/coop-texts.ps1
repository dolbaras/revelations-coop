# Texts of the installer and the launcher in the 13 languages of the game (the same set as the co-op lobby in the game).
# One source for both: the installer reads it from its own folder, the launcher from <game>\coop\coop-texts.ps1.
# '||' in a text = a line break; {0} {1} = arguments of L. Keys not translated fall back to English.
# Language codes: en ru de fr it es pt (Portugal) pt-br nl pl ja zh-cn (simplified) zh-tw (traditional).
$script:LANGS = [ordered]@{ "en" = "English"; "ru" = "Русский"; "de" = "Deutsch"; "fr" = "Français"; "it" = "Italiano"; "es" = "Español"; "pt" = "Português"; "pt-br" = "Português (Brasil)";
                            "nl" = "Nederlands"; "pl" = "Polski"; "ja" = "日本語"; "zh-cn" = "简体中文"; "zh-tw" = "繁體中文" }

# Windows' language (the UI language, then the format language) as one of the codes above; -Lang wins (also the game's own names: eng rus ger fre ita spa por ptB dut pol jpn chS chT).
function Pick-Lang([string] $want) {
    $alias = @{ "eng" = "en"; "rus" = "ru"; "ger" = "de"; "fre" = "fr"; "ita" = "it"; "spa" = "es"; "por" = "pt"; "ptb" = "pt-br"; "dut" = "nl"; "pol" = "pl"; "jpn" = "ja"; "chs" = "zh-cn"; "cht" = "zh-tw" }
    $cands = @()
    if ($want) { $cands += $want.ToLower() } else { $cands += (Get-UICulture).Name.ToLower(); $cands += (Get-Culture).Name.ToLower() }
    foreach ($c in $cands) {
        if ($alias.ContainsKey($c)) { return $alias[$c] }
        if ($script:LANGS.Contains($c)) { return $c }
        if ($c -match '^zh') { if ($c -match 'tw|hk|mo|hant') { return "zh-tw" } else { return "zh-cn" } }
        if ($c -eq "pt-pt") { return "pt" }
        if ($c -match '^pt') { return "pt-br" }
        $two = $c.Split('-')[0]
        if ($script:LANGS.Contains($two)) { return $two }
    }
    "en"
}

function L($key) {
    $text = $script:TEXTS[$script:lang][$key]
    if (-not $text) { $text = $script:TEXTS.en[$key] }
    if (-not $text) { return $key }
    $text = $text.Replace("||", "`n")
    if ($args.Count) { return ($text -f $args) }
    $text
}

$script:TEXTS = @{
 "en" = @{
  sub = 'Version {0}: two players on one PC, split screen'; folder = 'Game folder'; browse = 'Browse...'
  install = 'Install'; update = 'Update'; reinstall = 'Reinstall'; remove = 'Remove co-op'; cancel = 'Cancel'; close = 'Close'; copylog = 'Copy log'; openfolder = 'Open game folder'
  nogame = 'The game was not found. Select the folder that contains rerev.exe.'; noexe = 'There is no rerev.exe in this folder.'; found = 'Resident Evil Revelations found'
  badexe = 'This rerev.exe is not the supported Steam build 13298487. Nothing will be installed.'
  have = 'Co-op {0} is installed; this installer has {1}.'; same = 'Co-op {0} is already installed.'
  what = 'The installer copies the mod into the game folder and keeps the files it replaces in coop-backup. The game''s saves and settings are not touched.'
  need = 'Two players on one PC need two gamepads. In the main menu press F6 or BACK to open the co-op lobby.'
  s_check = 'Checking the game folder'; s_copy = 'Copying the mod'; s_finish = 'Writing the install record and the shortcuts'; s_rm = 'Removing the mod''s files'; s_restore = 'Putting back the game''s own files'; s_clean = 'Removing the backups and the mod folder'
  installing = 'Installing co-op {0}'; updated = 'Updated from {0} to {1}'; installed = 'Co-op {0} is installed'; removing = 'Removing co-op'; removed = 'Co-op removed'; ifail = 'Installation failed'; rfail = 'Removal failed'
  done = 'Start the game with the "Resident Evil Revelations Co-op" shortcut (desktop, Start menu): it also installs a newer version of the mod from GitHub first.||In the main menu press F6 or the BACK button to open the co-op lobby.'
  donenolink = 'Start the game as usual. In the main menu press F6 or the BACK button to open the co-op lobby.'
  gone = 'The mod was removed and the files it replaced were put back.'; login = 'The log is in {0}'; closegame = 'Close the game first.'
  another = 'Another installation of the mod is running: wait until it ends.'; noadmin = 'Windows did not allow writing to the game folder without administrator rights.'
  incomplete = 'The downloaded mod is incomplete: nothing was changed. Start the game again to retry.'; wait = 'Please wait until it ends.'
  pick = 'Select the Resident Evil Revelations folder'; confirm = 'Remove the co-op mod from {0}?'; kept = 'Original kept: '; copied = 'Copied {0} file(s).'; nrem = 'Removed {0} file(s).'; putback = 'Put back: '
  lnk = 'Shortcut: '; lnkdesc = 'Resident Evil Revelations with the co-op mod (installs a newer mod version first, if there is one)'; err = 'Error: '
  notmod = 'The co-op mod is not installed in {0}'; notgame = 'Not a Resident Evil Revelations folder: {0}'; nofiles = 'The "files" folder is missing next to the installer.'
  l_found = 'Co-op {0} is out (you have {1}). Downloading...'; l_verify = 'Checking the download...'; l_failed = 'The update could not be installed: {0}||The game starts with the version you have ({1}).'; l_bad = 'The download does not match its checksum.'
 }
 "ru" = @{
  sub = 'Версия {0}: два игрока на одном ПК, разделённый экран'; folder = 'Папка игры'; browse = 'Обзор...'
  install = 'Установить'; update = 'Обновить'; reinstall = 'Переустановить'; remove = 'Удалить кооператив'; cancel = 'Отмена'; close = 'Закрыть'; copylog = 'Копировать журнал'; openfolder = 'Открыть папку игры'
  nogame = 'Игра не найдена. Выберите папку, в которой лежит rerev.exe.'; noexe = 'В этой папке нет rerev.exe.'; found = 'Resident Evil Revelations найдена'
  badexe = 'Этот rerev.exe не относится к поддерживаемой сборке Steam 13298487. Ничего не будет установлено.'
  have = 'Кооператив {0} установлен; в этом установщике {1}.'; same = 'Кооператив {0} уже установлен.'
  what = 'Установщик копирует мод в папку игры, а файлы, которые он заменяет, сохраняет в coop-backup. Сохранения и настройки игры не затрагиваются.'
  need = 'Для двух игроков на одном ПК нужны два геймпада. В главном меню нажмите F6 или BACK, чтобы открыть лобби кооператива.'
  s_check = 'Проверка папки игры'; s_copy = 'Копирование мода'; s_finish = 'Запись сведений об установке и ярлыков'; s_rm = 'Удаление файлов мода'; s_restore = 'Возврат файлов самой игры'; s_clean = 'Удаление резервных копий и папки мода'
  installing = 'Установка кооператива {0}'; updated = 'Обновлено с {0} до {1}'; installed = 'Кооператив {0} установлен'; removing = 'Удаление кооператива'; removed = 'Кооператив удалён'; ifail = 'Установка не удалась'; rfail = 'Удаление не удалось'
  done = 'Запускайте игру ярлыком «Resident Evil Revelations Co-op» (рабочий стол, меню «Пуск»): он сначала ставит с GitHub более новую версию мода, если она есть.||В главном меню нажмите F6 или кнопку BACK, чтобы открыть лобби кооператива.'
  donenolink = 'Запускайте игру как обычно. В главном меню нажмите F6 или кнопку BACK, чтобы открыть лобби кооператива.'
  gone = 'Мод удалён, файлы, которые он заменял, возвращены.'; login = 'Журнал: {0}'; closegame = 'Сначала закройте игру.'
  another = 'Уже идёт другая установка мода: дождитесь её конца.'; noadmin = 'Windows не разрешила запись в папку игры без прав администратора.'
  incomplete = 'Загруженный мод неполон: ничего не изменено. Запустите игру ещё раз, чтобы повторить.'; wait = 'Подождите, пока всё закончится.'
  pick = 'Выберите папку Resident Evil Revelations'; confirm = 'Удалить кооперативный мод из {0}?'; kept = 'Оригинал сохранён: '; copied = 'Скопировано файлов: {0}.'; nrem = 'Удалено файлов: {0}.'; putback = 'Возвращено: '
  lnk = 'Ярлык: '; lnkdesc = 'Resident Evil Revelations с кооперативным модом (сначала ставит более новую версию мода, если она есть)'; err = 'Ошибка: '
  notmod = 'Кооперативный мод не установлен в {0}'; notgame = 'Это не папка Resident Evil Revelations: {0}'; nofiles = 'Рядом с установщиком нет папки «files».'
  l_found = 'Вышел кооператив {0} (у вас {1}). Загрузка...'; l_verify = 'Проверка загрузки...'; l_failed = 'Обновление не удалось установить: {0}||Игра запустится с вашей версией ({1}).'; l_bad = 'Загруженный файл не совпадает с контрольной суммой.'
 }
 "de" = @{
  sub = 'Version {0}: zwei Spieler an einem PC, geteilter Bildschirm'; folder = 'Spielordner'; browse = 'Durchsuchen...'
  install = 'Installieren'; update = 'Aktualisieren'; reinstall = 'Neu installieren'; remove = 'Koop entfernen'; cancel = 'Abbrechen'; close = 'Schließen'; copylog = 'Protokoll kopieren'; openfolder = 'Spielordner öffnen'
  nogame = 'Das Spiel wurde nicht gefunden. Wähle den Ordner mit rerev.exe.'; noexe = 'In diesem Ordner liegt keine rerev.exe.'; found = 'Resident Evil Revelations gefunden'
  badexe = 'Diese rerev.exe ist nicht der unterstützte Steam-Build 13298487. Es wird nichts installiert.'
  have = 'Koop {0} ist installiert; dieser Installer hat {1}.'; same = 'Koop {0} ist bereits installiert.'
  what = 'Der Installer kopiert die Mod in den Spielordner und bewahrt die ersetzten Dateien in coop-backup auf. Spielstände und Einstellungen des Spiels bleiben unberührt.'
  need = 'Zwei Spieler an einem PC brauchen zwei Gamepads. Drücke im Hauptmenü F6 oder BACK, um die Koop-Lobby zu öffnen.'
  s_check = 'Spielordner wird geprüft'; s_copy = 'Mod wird kopiert'; s_finish = 'Installationsprotokoll und Verknüpfungen werden geschrieben'; s_rm = 'Dateien der Mod werden entfernt'; s_restore = 'Originaldateien des Spiels werden zurückgelegt'; s_clean = 'Sicherungen und Mod-Ordner werden entfernt'
  installing = 'Koop {0} wird installiert'; updated = 'Von {0} auf {1} aktualisiert'; installed = 'Koop {0} ist installiert'; removing = 'Koop wird entfernt'; removed = 'Koop entfernt'; ifail = 'Installation fehlgeschlagen'; rfail = 'Entfernen fehlgeschlagen'
  done = 'Starte das Spiel mit der Verknüpfung "Resident Evil Revelations Co-op" (Desktop, Startmenü): Sie installiert vorher eine neuere Version der Mod von GitHub.||Drücke im Hauptmenü F6 oder die BACK-Taste, um die Koop-Lobby zu öffnen.'
  donenolink = 'Starte das Spiel wie gewohnt. Drücke im Hauptmenü F6 oder die BACK-Taste, um die Koop-Lobby zu öffnen.'
  gone = 'Die Mod wurde entfernt, die ersetzten Dateien wurden zurückgelegt.'; login = 'Das Protokoll liegt in {0}'; closegame = 'Beende zuerst das Spiel.'
  another = 'Eine andere Installation der Mod läuft: warte, bis sie fertig ist.'; noadmin = 'Windows erlaubt das Schreiben in den Spielordner nur mit Administratorrechten.'
  incomplete = 'Die heruntergeladene Mod ist unvollständig: nichts wurde geändert. Starte das Spiel erneut, um es nochmal zu versuchen.'; wait = 'Bitte warte, bis es fertig ist.'
  pick = 'Wähle den Ordner von Resident Evil Revelations'; confirm = 'Die Koop-Mod aus {0} entfernen?'; kept = 'Original gesichert: '; copied = '{0} Datei(en) kopiert.'; nrem = '{0} Datei(en) entfernt.'; putback = 'Zurückgelegt: '
  lnk = 'Verknüpfung: '; lnkdesc = 'Resident Evil Revelations mit der Koop-Mod (installiert zuerst eine neuere Mod-Version, falls vorhanden)'; err = 'Fehler: '
  notmod = 'Die Koop-Mod ist in {0} nicht installiert'; notgame = 'Kein Resident-Evil-Revelations-Ordner: {0}'; nofiles = 'Der Ordner "files" fehlt neben dem Installer.'
  l_found = 'Koop {0} ist erschienen (du hast {1}). Download läuft...'; l_verify = 'Download wird geprüft...'; l_failed = 'Das Update konnte nicht installiert werden: {0}||Das Spiel startet mit deiner Version ({1}).'; l_bad = 'Der Download passt nicht zur Prüfsumme.'
 }
 "fr" = @{
  sub = 'Version {0} : deux joueurs sur un PC, écran partagé'; folder = 'Dossier du jeu'; browse = 'Parcourir...'
  install = 'Installer'; update = 'Mettre à jour'; reinstall = 'Réinstaller'; remove = 'Retirer le coop'; cancel = 'Annuler'; close = 'Fermer'; copylog = 'Copier le journal'; openfolder = 'Ouvrir le dossier du jeu'
  nogame = 'Le jeu est introuvable. Choisissez le dossier qui contient rerev.exe.'; noexe = 'Il n''y a pas de rerev.exe dans ce dossier.'; found = 'Resident Evil Revelations trouvé'
  badexe = 'Ce rerev.exe n''est pas la version Steam 13298487 prise en charge. Rien ne sera installé.'
  have = 'Le coop {0} est installé ; cet installateur contient {1}.'; same = 'Le coop {0} est déjà installé.'
  what = 'L''installateur copie le mod dans le dossier du jeu et garde les fichiers qu''il remplace dans coop-backup. Les sauvegardes et réglages du jeu ne sont pas touchés.'
  need = 'Deux joueurs sur un PC ont besoin de deux manettes. Dans le menu principal, appuyez sur F6 ou BACK pour ouvrir le salon coop.'
  s_check = 'Vérification du dossier du jeu'; s_copy = 'Copie du mod'; s_finish = 'Écriture du fichier d''installation et des raccourcis'; s_rm = 'Suppression des fichiers du mod'; s_restore = 'Remise des fichiers d''origine du jeu'; s_clean = 'Suppression des copies de sauvegarde et du dossier du mod'
  installing = 'Installation du coop {0}'; updated = 'Mis à jour de {0} vers {1}'; installed = 'Le coop {0} est installé'; removing = 'Suppression du coop'; removed = 'Coop retiré'; ifail = 'Échec de l''installation'; rfail = 'Échec de la suppression'
  done = 'Lancez le jeu avec le raccourci « Resident Evil Revelations Co-op » (bureau, menu Démarrer) : il installe d''abord une version plus récente du mod depuis GitHub, s''il y en a une.||Dans le menu principal, appuyez sur F6 ou sur le bouton BACK pour ouvrir le salon coop.'
  donenolink = 'Lancez le jeu comme d''habitude. Dans le menu principal, appuyez sur F6 ou sur le bouton BACK pour ouvrir le salon coop.'
  gone = 'Le mod a été retiré et les fichiers qu''il remplaçait ont été remis.'; login = 'Le journal se trouve dans {0}'; closegame = 'Fermez d''abord le jeu.'
  another = 'Une autre installation du mod est en cours : attendez qu''elle se termine.'; noadmin = 'Windows n''autorise pas l''écriture dans le dossier du jeu sans droits d''administrateur.'
  incomplete = 'Le mod téléchargé est incomplet : rien n''a été modifié. Relancez le jeu pour réessayer.'; wait = 'Veuillez patienter jusqu''à la fin.'
  pick = 'Choisissez le dossier de Resident Evil Revelations'; confirm = 'Retirer le mod coop de {0} ?'; kept = 'Original conservé : '; copied = '{0} fichier(s) copié(s).'; nrem = '{0} fichier(s) supprimé(s).'; putback = 'Remis en place : '
  lnk = 'Raccourci : '; lnkdesc = 'Resident Evil Revelations avec le mod coop (installe d''abord une version plus récente du mod, s''il y en a une)'; err = 'Erreur : '
  notmod = 'Le mod coop n''est pas installé dans {0}'; notgame = 'Ce n''est pas un dossier Resident Evil Revelations : {0}'; nofiles = 'Le dossier « files » manque à côté de l''installateur.'
  l_found = 'Le coop {0} est sorti (vous avez {1}). Téléchargement...'; l_verify = 'Vérification du téléchargement...'; l_failed = 'La mise à jour n''a pas pu être installée : {0}||Le jeu démarre avec votre version ({1}).'; l_bad = 'Le téléchargement ne correspond pas à sa somme de contrôle.'
 }
 "it" = @{
  sub = 'Versione {0}: due giocatori su un PC, schermo diviso'; folder = 'Cartella del gioco'; browse = 'Sfoglia...'
  install = 'Installa'; update = 'Aggiorna'; reinstall = 'Reinstalla'; remove = 'Rimuovi il co-op'; cancel = 'Annulla'; close = 'Chiudi'; copylog = 'Copia il registro'; openfolder = 'Apri la cartella del gioco'
  nogame = 'Gioco non trovato. Scegli la cartella che contiene rerev.exe.'; noexe = 'In questa cartella non c''è rerev.exe.'; found = 'Resident Evil Revelations trovato'
  badexe = 'Questo rerev.exe non è la build Steam 13298487 supportata. Non verrà installato nulla.'
  have = 'Il co-op {0} è installato; questo installer contiene {1}.'; same = 'Il co-op {0} è già installato.'
  what = 'L''installer copia la mod nella cartella del gioco e conserva in coop-backup i file che sostituisce. Salvataggi e impostazioni del gioco non vengono toccati.'
  need = 'Due giocatori su un PC hanno bisogno di due gamepad. Nel menu principale premi F6 o BACK per aprire la lobby co-op.'
  s_check = 'Controllo della cartella del gioco'; s_copy = 'Copia della mod'; s_finish = 'Scrittura del registro di installazione e dei collegamenti'; s_rm = 'Rimozione dei file della mod'; s_restore = 'Ripristino dei file originali del gioco'; s_clean = 'Rimozione delle copie di sicurezza e della cartella della mod'
  installing = 'Installazione del co-op {0}'; updated = 'Aggiornato da {0} a {1}'; installed = 'Il co-op {0} è installato'; removing = 'Rimozione del co-op'; removed = 'Co-op rimosso'; ifail = 'Installazione non riuscita'; rfail = 'Rimozione non riuscita'
  done = 'Avvia il gioco con il collegamento "Resident Evil Revelations Co-op" (desktop, menu Start): prima installa da GitHub una versione più recente della mod, se c''è.||Nel menu principale premi F6 o il tasto BACK per aprire la lobby co-op.'
  donenolink = 'Avvia il gioco come al solito. Nel menu principale premi F6 o il tasto BACK per aprire la lobby co-op.'
  gone = 'La mod è stata rimossa e i file che sostituiva sono stati rimessi a posto.'; login = 'Il registro è in {0}'; closegame = 'Chiudi prima il gioco.'
  another = 'È in corso un''altra installazione della mod: attendi che finisca.'; noadmin = 'Windows non consente di scrivere nella cartella del gioco senza diritti di amministratore.'
  incomplete = 'La mod scaricata è incompleta: non è stato modificato nulla. Riavvia il gioco per riprovare.'; wait = 'Attendi che finisca.'
  pick = 'Scegli la cartella di Resident Evil Revelations'; confirm = 'Rimuovere la mod co-op da {0}?'; kept = 'Originale conservato: '; copied = '{0} file copiati.'; nrem = '{0} file rimossi.'; putback = 'Ripristinato: '
  lnk = 'Collegamento: '; lnkdesc = 'Resident Evil Revelations con la mod co-op (prima installa una versione più recente della mod, se c''è)'; err = 'Errore: '
  notmod = 'La mod co-op non è installata in {0}'; notgame = 'Non è una cartella di Resident Evil Revelations: {0}'; nofiles = 'La cartella "files" manca accanto all''installer.'
  l_found = 'È uscito il co-op {0} (hai {1}). Download in corso...'; l_verify = 'Verifica del download...'; l_failed = 'Impossibile installare l''aggiornamento: {0}||Il gioco parte con la versione che hai ({1}).'; l_bad = 'Il download non corrisponde al suo checksum.'
 }
 "es" = @{
  sub = 'Versión {0}: dos jugadores en un PC, pantalla dividida'; folder = 'Carpeta del juego'; browse = 'Examinar...'
  install = 'Instalar'; update = 'Actualizar'; reinstall = 'Reinstalar'; remove = 'Quitar el cooperativo'; cancel = 'Cancelar'; close = 'Cerrar'; copylog = 'Copiar el registro'; openfolder = 'Abrir la carpeta del juego'
  nogame = 'No se encontró el juego. Elige la carpeta que contiene rerev.exe.'; noexe = 'No hay rerev.exe en esta carpeta.'; found = 'Resident Evil Revelations encontrado'
  badexe = 'Este rerev.exe no es la compilación de Steam 13298487 compatible. No se instalará nada.'
  have = 'El cooperativo {0} está instalado; este instalador trae {1}.'; same = 'El cooperativo {0} ya está instalado.'
  what = 'El instalador copia el mod en la carpeta del juego y guarda en coop-backup los archivos que reemplaza. Las partidas guardadas y los ajustes del juego no se tocan.'
  need = 'Dos jugadores en un PC necesitan dos mandos. En el menú principal pulsa F6 o BACK para abrir la sala cooperativa.'
  s_check = 'Comprobando la carpeta del juego'; s_copy = 'Copiando el mod'; s_finish = 'Escribiendo el registro de instalación y los accesos directos'; s_rm = 'Quitando los archivos del mod'; s_restore = 'Devolviendo los archivos originales del juego'; s_clean = 'Quitando las copias de seguridad y la carpeta del mod'
  installing = 'Instalando el cooperativo {0}'; updated = 'Actualizado de {0} a {1}'; installed = 'El cooperativo {0} está instalado'; removing = 'Quitando el cooperativo'; removed = 'Cooperativo quitado'; ifail = 'Falló la instalación'; rfail = 'Falló la eliminación'
  done = 'Inicia el juego con el acceso directo "Resident Evil Revelations Co-op" (escritorio, menú Inicio): antes instala desde GitHub una versión más reciente del mod, si la hay.||En el menú principal pulsa F6 o el botón BACK para abrir la sala cooperativa.'
  donenolink = 'Inicia el juego como siempre. En el menú principal pulsa F6 o el botón BACK para abrir la sala cooperativa.'
  gone = 'El mod se quitó y los archivos que reemplazaba se devolvieron.'; login = 'El registro está en {0}'; closegame = 'Cierra primero el juego.'
  another = 'Hay otra instalación del mod en curso: espera a que termine.'; noadmin = 'Windows no permite escribir en la carpeta del juego sin derechos de administrador.'
  incomplete = 'El mod descargado está incompleto: no se cambió nada. Inicia el juego otra vez para reintentarlo.'; wait = 'Espera a que termine.'
  pick = 'Elige la carpeta de Resident Evil Revelations'; confirm = '¿Quitar el mod cooperativo de {0}?'; kept = 'Original conservado: '; copied = '{0} archivo(s) copiado(s).'; nrem = '{0} archivo(s) quitado(s).'; putback = 'Devuelto: '
  lnk = 'Acceso directo: '; lnkdesc = 'Resident Evil Revelations con el mod cooperativo (antes instala una versión más reciente del mod, si la hay)'; err = 'Error: '
  notmod = 'El mod cooperativo no está instalado en {0}'; notgame = 'No es una carpeta de Resident Evil Revelations: {0}'; nofiles = 'Falta la carpeta "files" junto al instalador.'
  l_found = 'Ya está el cooperativo {0} (tienes {1}). Descargando...'; l_verify = 'Comprobando la descarga...'; l_failed = 'No se pudo instalar la actualización: {0}||El juego arranca con la versión que tienes ({1}).'; l_bad = 'La descarga no coincide con su suma de comprobación.'
 }
 "pt" = @{
  sub = 'Versão {0}: dois jogadores num PC, ecrã dividido'; folder = 'Pasta do jogo'; browse = 'Procurar...'
  install = 'Instalar'; update = 'Atualizar'; reinstall = 'Reinstalar'; remove = 'Remover o cooperativo'; cancel = 'Cancelar'; close = 'Fechar'; copylog = 'Copiar o registo'; openfolder = 'Abrir a pasta do jogo'
  nogame = 'O jogo não foi encontrado. Escolha a pasta que contém rerev.exe.'; noexe = 'Não há rerev.exe nesta pasta.'; found = 'Resident Evil Revelations encontrado'
  badexe = 'Este rerev.exe não é a compilação Steam 13298487 suportada. Nada será instalado.'
  have = 'O cooperativo {0} está instalado; este instalador tem {1}.'; same = 'O cooperativo {0} já está instalado.'
  what = 'O instalador copia o mod para a pasta do jogo e guarda em coop-backup os ficheiros que substitui. Os jogos guardados e as definições do jogo não são tocados.'
  need = 'Dois jogadores num PC precisam de dois comandos. No menu principal prima F6 ou BACK para abrir a sala cooperativa.'
  s_check = 'A verificar a pasta do jogo'; s_copy = 'A copiar o mod'; s_finish = 'A escrever o registo de instalação e os atalhos'; s_rm = 'A remover os ficheiros do mod'; s_restore = 'A repor os ficheiros originais do jogo'; s_clean = 'A remover as cópias de segurança e a pasta do mod'
  installing = 'A instalar o cooperativo {0}'; updated = 'Atualizado de {0} para {1}'; installed = 'O cooperativo {0} está instalado'; removing = 'A remover o cooperativo'; removed = 'Cooperativo removido'; ifail = 'A instalação falhou'; rfail = 'A remoção falhou'
  done = 'Inicie o jogo com o atalho "Resident Evil Revelations Co-op" (ambiente de trabalho, menu Iniciar): primeiro instala do GitHub uma versão mais recente do mod, se existir.||No menu principal prima F6 ou o botão BACK para abrir a sala cooperativa.'
  donenolink = 'Inicie o jogo como habitualmente. No menu principal prima F6 ou o botão BACK para abrir a sala cooperativa.'
  gone = 'O mod foi removido e os ficheiros que substituía foram repostos.'; login = 'O registo está em {0}'; closegame = 'Feche primeiro o jogo.'
  another = 'Está a decorrer outra instalação do mod: espere que termine.'; noadmin = 'O Windows não permite escrever na pasta do jogo sem direitos de administrador.'
  incomplete = 'O mod transferido está incompleto: nada foi alterado. Inicie o jogo outra vez para tentar de novo.'; wait = 'Aguarde até terminar.'
  pick = 'Escolha a pasta do Resident Evil Revelations'; confirm = 'Remover o mod cooperativo de {0}?'; kept = 'Original guardado: '; copied = '{0} ficheiro(s) copiado(s).'; nrem = '{0} ficheiro(s) removido(s).'; putback = 'Reposto: '
  lnk = 'Atalho: '; lnkdesc = 'Resident Evil Revelations com o mod cooperativo (primeiro instala uma versão mais recente do mod, se existir)'; err = 'Erro: '
  notmod = 'O mod cooperativo não está instalado em {0}'; notgame = 'Não é uma pasta do Resident Evil Revelations: {0}'; nofiles = 'Falta a pasta "files" junto ao instalador.'
  l_found = 'Saiu o cooperativo {0} (tem {1}). A transferir...'; l_verify = 'A verificar a transferência...'; l_failed = 'Não foi possível instalar a atualização: {0}||O jogo arranca com a versão que tem ({1}).'; l_bad = 'A transferência não corresponde à sua soma de verificação.'
 }
 "pt-br" = @{
  sub = 'Versão {0}: dois jogadores em um PC, tela dividida'; folder = 'Pasta do jogo'; browse = 'Procurar...'
  install = 'Instalar'; update = 'Atualizar'; reinstall = 'Reinstalar'; remove = 'Remover o cooperativo'; cancel = 'Cancelar'; close = 'Fechar'; copylog = 'Copiar o registro'; openfolder = 'Abrir a pasta do jogo'
  nogame = 'O jogo não foi encontrado. Escolha a pasta que contém rerev.exe.'; noexe = 'Não há rerev.exe nesta pasta.'; found = 'Resident Evil Revelations encontrado'
  badexe = 'Este rerev.exe não é a versão Steam 13298487 compatível. Nada será instalado.'
  have = 'O cooperativo {0} está instalado; este instalador tem {1}.'; same = 'O cooperativo {0} já está instalado.'
  what = 'O instalador copia o mod para a pasta do jogo e guarda em coop-backup os arquivos que substitui. Os jogos salvos e as configurações do jogo não são tocados.'
  need = 'Dois jogadores em um PC precisam de dois controles. No menu principal, pressione F6 ou BACK para abrir o lobby cooperativo.'
  s_check = 'Verificando a pasta do jogo'; s_copy = 'Copiando o mod'; s_finish = 'Gravando o registro de instalação e os atalhos'; s_rm = 'Removendo os arquivos do mod'; s_restore = 'Devolvendo os arquivos originais do jogo'; s_clean = 'Removendo os backups e a pasta do mod'
  installing = 'Instalando o cooperativo {0}'; updated = 'Atualizado de {0} para {1}'; installed = 'O cooperativo {0} está instalado'; removing = 'Removendo o cooperativo'; removed = 'Cooperativo removido'; ifail = 'A instalação falhou'; rfail = 'A remoção falhou'
  done = 'Inicie o jogo pelo atalho "Resident Evil Revelations Co-op" (área de trabalho, menu Iniciar): ele primeiro instala do GitHub uma versão mais recente do mod, se houver.||No menu principal, pressione F6 ou o botão BACK para abrir o lobby cooperativo.'
  donenolink = 'Inicie o jogo como de costume. No menu principal, pressione F6 ou o botão BACK para abrir o lobby cooperativo.'
  gone = 'O mod foi removido e os arquivos que ele substituía foram devolvidos.'; login = 'O registro está em {0}'; closegame = 'Feche o jogo primeiro.'
  another = 'Outra instalação do mod está em andamento: aguarde o fim.'; noadmin = 'O Windows não permitiu gravar na pasta do jogo sem direitos de administrador.'
  incomplete = 'O mod baixado está incompleto: nada foi alterado. Inicie o jogo de novo para tentar outra vez.'; wait = 'Aguarde até terminar.'
  pick = 'Escolha a pasta do Resident Evil Revelations'; confirm = 'Remover o mod cooperativo de {0}?'; kept = 'Original guardado: '; copied = '{0} arquivo(s) copiado(s).'; nrem = '{0} arquivo(s) removido(s).'; putback = 'Devolvido: '
  lnk = 'Atalho: '; lnkdesc = 'Resident Evil Revelations com o mod cooperativo (primeiro instala uma versão mais recente do mod, se houver)'; err = 'Erro: '
  notmod = 'O mod cooperativo não está instalado em {0}'; notgame = 'Não é uma pasta do Resident Evil Revelations: {0}'; nofiles = 'A pasta "files" está faltando ao lado do instalador.'
  l_found = 'O cooperativo {0} foi lançado (você tem {1}). Baixando...'; l_verify = 'Verificando o download...'; l_failed = 'Não foi possível instalar a atualização: {0}||O jogo inicia com a versão que você tem ({1}).'; l_bad = 'O download não corresponde ao seu checksum.'
 }
 "nl" = @{
  sub = 'Versie {0}: twee spelers op één pc, gesplitst scherm'; folder = 'Spelmap'; browse = 'Bladeren...'
  install = 'Installeren'; update = 'Bijwerken'; reinstall = 'Opnieuw installeren'; remove = 'Coöp verwijderen'; cancel = 'Annuleren'; close = 'Sluiten'; copylog = 'Logboek kopiëren'; openfolder = 'Spelmap openen'
  nogame = 'Het spel is niet gevonden. Kies de map met rerev.exe.'; noexe = 'In deze map staat geen rerev.exe.'; found = 'Resident Evil Revelations gevonden'
  badexe = 'Deze rerev.exe is niet de ondersteunde Steam-build 13298487. Er wordt niets geïnstalleerd.'
  have = 'Coöp {0} is geïnstalleerd; dit installatieprogramma heeft {1}.'; same = 'Coöp {0} is al geïnstalleerd.'
  what = 'Het installatieprogramma kopieert de mod naar de spelmap en bewaart de bestanden die het vervangt in coop-backup. Savegames en instellingen van het spel blijven ongemoeid.'
  need = 'Twee spelers op één pc hebben twee controllers nodig. Druk in het hoofdmenu op F6 of BACK om de coöp-lobby te openen.'
  s_check = 'Spelmap controleren'; s_copy = 'Mod kopiëren'; s_finish = 'Installatiegegevens en snelkoppelingen schrijven'; s_rm = 'Bestanden van de mod verwijderen'; s_restore = 'Originele bestanden van het spel terugzetten'; s_clean = 'Back-ups en modmap verwijderen'
  installing = 'Coöp {0} installeren'; updated = 'Bijgewerkt van {0} naar {1}'; installed = 'Coöp {0} is geïnstalleerd'; removing = 'Coöp verwijderen'; removed = 'Coöp verwijderd'; ifail = 'Installatie mislukt'; rfail = 'Verwijderen mislukt'
  done = 'Start het spel met de snelkoppeling "Resident Evil Revelations Co-op" (bureaublad, Startmenu): die installeert eerst een nieuwere versie van de mod van GitHub, als die er is.||Druk in het hoofdmenu op F6 of de BACK-knop om de coöp-lobby te openen.'
  donenolink = 'Start het spel zoals gewoonlijk. Druk in het hoofdmenu op F6 of de BACK-knop om de coöp-lobby te openen.'
  gone = 'De mod is verwijderd en de bestanden die hij verving zijn teruggezet.'; login = 'Het logboek staat in {0}'; closegame = 'Sluit eerst het spel.'
  another = 'Er loopt al een andere installatie van de mod: wacht tot die klaar is.'; noadmin = 'Windows staat schrijven naar de spelmap zonder beheerdersrechten niet toe.'
  incomplete = 'De gedownloade mod is onvolledig: er is niets gewijzigd. Start het spel opnieuw om het nog eens te proberen.'; wait = 'Wacht tot het klaar is.'
  pick = 'Kies de map van Resident Evil Revelations'; confirm = 'De coöp-mod uit {0} verwijderen?'; kept = 'Origineel bewaard: '; copied = '{0} bestand(en) gekopieerd.'; nrem = '{0} bestand(en) verwijderd.'; putback = 'Teruggezet: '
  lnk = 'Snelkoppeling: '; lnkdesc = 'Resident Evil Revelations met de coöp-mod (installeert eerst een nieuwere modversie, als die er is)'; err = 'Fout: '
  notmod = 'De coöp-mod is niet geïnstalleerd in {0}'; notgame = 'Geen Resident Evil Revelations-map: {0}'; nofiles = 'De map "files" ontbreekt naast het installatieprogramma.'
  l_found = 'Coöp {0} is uit (je hebt {1}). Downloaden...'; l_verify = 'Download controleren...'; l_failed = 'De update kon niet worden geïnstalleerd: {0}||Het spel start met de versie die je hebt ({1}).'; l_bad = 'De download komt niet overeen met zijn controlesom.'
 }
 "pl" = @{
  sub = 'Wersja {0}: dwóch graczy na jednym PC, podzielony ekran'; folder = 'Folder gry'; browse = 'Przeglądaj...'
  install = 'Zainstaluj'; update = 'Zaktualizuj'; reinstall = 'Zainstaluj ponownie'; remove = 'Usuń tryb kooperacji'; cancel = 'Anuluj'; close = 'Zamknij'; copylog = 'Kopiuj dziennik'; openfolder = 'Otwórz folder gry'
  nogame = 'Nie znaleziono gry. Wybierz folder z plikiem rerev.exe.'; noexe = 'W tym folderze nie ma pliku rerev.exe.'; found = 'Znaleziono Resident Evil Revelations'
  badexe = 'Ten rerev.exe nie jest obsługiwaną wersją Steam 13298487. Nic nie zostanie zainstalowane.'
  have = 'Kooperacja {0} jest zainstalowana; ten instalator zawiera {1}.'; same = 'Kooperacja {0} jest już zainstalowana.'
  what = 'Instalator kopiuje moda do folderu gry, a pliki, które zastępuje, zachowuje w coop-backup. Zapisy gry i ustawienia nie są ruszane.'
  need = 'Dwóch graczy na jednym PC potrzebuje dwóch padów. W menu głównym naciśnij F6 lub BACK, aby otworzyć lobby kooperacji.'
  s_check = 'Sprawdzanie folderu gry'; s_copy = 'Kopiowanie moda'; s_finish = 'Zapisywanie danych instalacji i skrótów'; s_rm = 'Usuwanie plików moda'; s_restore = 'Przywracanie oryginalnych plików gry'; s_clean = 'Usuwanie kopii zapasowych i folderu moda'
  installing = 'Instalowanie kooperacji {0}'; updated = 'Zaktualizowano z {0} do {1}'; installed = 'Kooperacja {0} jest zainstalowana'; removing = 'Usuwanie kooperacji'; removed = 'Kooperacja usunięta'; ifail = 'Instalacja nie powiodła się'; rfail = 'Usuwanie nie powiodło się'
  done = 'Uruchom grę skrótem „Resident Evil Revelations Co-op” (pulpit, menu Start): najpierw zainstaluje z GitHuba nowszą wersję moda, jeśli jest.||W menu głównym naciśnij F6 lub przycisk BACK, aby otworzyć lobby kooperacji.'
  donenolink = 'Uruchom grę jak zwykle. W menu głównym naciśnij F6 lub przycisk BACK, aby otworzyć lobby kooperacji.'
  gone = 'Mod został usunięty, a pliki, które zastępował, przywrócone.'; login = 'Dziennik znajduje się w {0}'; closegame = 'Najpierw zamknij grę.'
  another = 'Trwa inna instalacja moda: poczekaj, aż się skończy.'; noadmin = 'Windows nie pozwala zapisywać w folderze gry bez uprawnień administratora.'
  incomplete = 'Pobrany mod jest niekompletny: nic nie zmieniono. Uruchom grę ponownie, aby spróbować jeszcze raz.'; wait = 'Poczekaj na zakończenie.'
  pick = 'Wybierz folder Resident Evil Revelations'; confirm = 'Usunąć moda kooperacji z {0}?'; kept = 'Zachowano oryginał: '; copied = 'Skopiowano plików: {0}.'; nrem = 'Usunięto plików: {0}.'; putback = 'Przywrócono: '
  lnk = 'Skrót: '; lnkdesc = 'Resident Evil Revelations z modem kooperacji (najpierw instaluje nowszą wersję moda, jeśli jest)'; err = 'Błąd: '
  notmod = 'Mod kooperacji nie jest zainstalowany w {0}'; notgame = 'To nie jest folder Resident Evil Revelations: {0}'; nofiles = 'Obok instalatora brakuje folderu „files”.'
  l_found = 'Wyszła kooperacja {0} (masz {1}). Pobieranie...'; l_verify = 'Sprawdzanie pobranego pliku...'; l_failed = 'Nie udało się zainstalować aktualizacji: {0}||Gra uruchomi się z posiadaną wersją ({1}).'; l_bad = 'Pobrany plik nie zgadza się z sumą kontrolną.'
 }
 "ja" = @{
  sub = 'バージョン {0}：1台のPCで2人プレイ（分割画面）'; folder = 'ゲームのフォルダー'; browse = '参照...'
  install = 'インストール'; update = '更新'; reinstall = '再インストール'; remove = 'Co-opを削除'; cancel = 'キャンセル'; close = '閉じる'; copylog = 'ログをコピー'; openfolder = 'ゲームのフォルダーを開く'
  nogame = 'ゲームが見つかりません。rerev.exe があるフォルダーを選んでください。'; noexe = 'このフォルダーに rerev.exe がありません。'; found = 'Resident Evil Revelations が見つかりました'
  badexe = 'この rerev.exe は対応する Steam ビルド 13298487 ではありません。何もインストールしません。'
  have = 'Co-op {0} がインストール済みです（このインストーラーは {1}）。'; same = 'Co-op {0} はすでにインストールされています。'
  what = 'インストーラーはMODをゲームのフォルダーにコピーし、置き換えるファイルを coop-backup に保存します。ゲームのセーブデータと設定には触れません。'
  need = '1台のPCで2人プレイするにはゲームパッドが2つ必要です。メインメニューで F6 または BACK を押すと Co-op ロビーが開きます。'
  s_check = 'ゲームのフォルダーを確認中'; s_copy = 'MODをコピー中'; s_finish = 'インストール情報とショートカットを書き込み中'; s_rm = 'MODのファイルを削除中'; s_restore = 'ゲーム本来のファイルを元に戻し中'; s_clean = 'バックアップとMODのフォルダーを削除中'
  installing = 'Co-op {0} をインストール中'; updated = '{0} から {1} に更新しました'; installed = 'Co-op {0} をインストールしました'; removing = 'Co-op を削除中'; removed = 'Co-op を削除しました'; ifail = 'インストールに失敗しました'; rfail = '削除に失敗しました'
  done = '「Resident Evil Revelations Co-op」ショートカット（デスクトップ／スタートメニュー）からゲームを起動してください。新しいMODがあれば、先にGitHubからインストールします。||メインメニューで F6 または BACK ボタンを押すと Co-op ロビーが開きます。'
  donenolink = 'いつも通りゲームを起動してください。メインメニューで F6 または BACK ボタンを押すと Co-op ロビーが開きます。'
  gone = 'MODを削除し、置き換えていたファイルを元に戻しました。'; login = 'ログの場所: {0}'; closegame = '先にゲームを終了してください。'
  another = '別のMODのインストールが実行中です。終わるまでお待ちください。'; noadmin = '管理者権限がないと、Windows はゲームのフォルダーへの書き込みを許可しません。'
  incomplete = 'ダウンロードしたMODが不完全です。何も変更していません。ゲームをもう一度起動してやり直してください。'; wait = '終わるまでお待ちください。'
  pick = 'Resident Evil Revelations のフォルダーを選んでください'; confirm = '{0} から Co-op MODを削除しますか？'; kept = '元のファイルを保存: '; copied = '{0} 個のファイルをコピーしました。'; nrem = '{0} 個のファイルを削除しました。'; putback = '元に戻しました: '
  lnk = 'ショートカット: '; lnkdesc = 'Co-op MOD付きの Resident Evil Revelations（新しいMODがあれば先にインストールします）'; err = 'エラー: '
  notmod = '{0} に Co-op MODはインストールされていません'; notgame = 'Resident Evil Revelations のフォルダーではありません: {0}'; nofiles = 'インストーラーの隣に「files」フォルダーがありません。'
  l_found = 'Co-op {0} が公開されました（現在 {1}）。ダウンロード中...'; l_verify = 'ダウンロードを確認中...'; l_failed = '更新をインストールできませんでした: {0}||現在のバージョン（{1}）でゲームを起動します。'; l_bad = 'ダウンロードしたファイルがチェックサムと一致しません。'
 }
 "zh-cn" = @{
  sub = '版本 {0}：一台电脑两名玩家，分屏'; folder = '游戏文件夹'; browse = '浏览...'
  install = '安装'; update = '更新'; reinstall = '重新安装'; remove = '移除合作模式'; cancel = '取消'; close = '关闭'; copylog = '复制日志'; openfolder = '打开游戏文件夹'
  nogame = '未找到游戏。请选择包含 rerev.exe 的文件夹。'; noexe = '此文件夹中没有 rerev.exe。'; found = '已找到 Resident Evil Revelations'
  badexe = '此 rerev.exe 不是受支持的 Steam 版本 13298487。不会安装任何内容。'
  have = '已安装合作模式 {0}；此安装程序为 {1}。'; same = '合作模式 {0} 已安装。'
  what = '安装程序会把模组复制到游戏文件夹，并把被替换的文件保存在 coop-backup 中。游戏存档和设置不会被改动。'
  need = '一台电脑上两名玩家需要两个手柄。在主菜单按 F6 或 BACK 打开合作大厅。'
  s_check = '正在检查游戏文件夹'; s_copy = '正在复制模组'; s_finish = '正在写入安装记录和快捷方式'; s_rm = '正在删除模组文件'; s_restore = '正在还原游戏自带文件'; s_clean = '正在删除备份和模组文件夹'
  installing = '正在安装合作模式 {0}'; updated = '已从 {0} 更新到 {1}'; installed = '合作模式 {0} 已安装'; removing = '正在移除合作模式'; removed = '合作模式已移除'; ifail = '安装失败'; rfail = '移除失败'
  done = '请通过“Resident Evil Revelations Co-op”快捷方式（桌面、开始菜单）启动游戏：如有更新版本，它会先从 GitHub 安装。||在主菜单按 F6 或 BACK 键打开合作大厅。'
  donenolink = '像平常一样启动游戏。在主菜单按 F6 或 BACK 键打开合作大厅。'
  gone = '模组已移除，被替换的文件已还原。'; login = '日志位于 {0}'; closegame = '请先关闭游戏。'
  another = '另一个模组安装正在进行：请等它结束。'; noadmin = '没有管理员权限，Windows 不允许写入游戏文件夹。'
  incomplete = '下载的模组不完整：未做任何更改。请重新启动游戏再试一次。'; wait = '请等待完成。'
  pick = '请选择 Resident Evil Revelations 文件夹'; confirm = '从 {0} 移除合作模组？'; kept = '已保留原文件：'; copied = '已复制 {0} 个文件。'; nrem = '已删除 {0} 个文件。'; putback = '已还原：'
  lnk = '快捷方式：'; lnkdesc = '带合作模组的 Resident Evil Revelations（如有更新版本，会先安装）'; err = '错误：'
  notmod = '{0} 中未安装合作模组'; notgame = '不是 Resident Evil Revelations 文件夹：{0}'; nofiles = '安装程序旁边缺少“files”文件夹。'
  l_found = '合作模式 {0} 已发布（您当前为 {1}）。正在下载...'; l_verify = '正在校验下载...'; l_failed = '无法安装更新：{0}||游戏将以当前版本（{1}）启动。'; l_bad = '下载的文件与其校验和不符。'
 }
 "zh-tw" = @{
  sub = '版本 {0}：一台電腦兩名玩家，分割畫面'; folder = '遊戲資料夾'; browse = '瀏覽...'
  install = '安裝'; update = '更新'; reinstall = '重新安裝'; remove = '移除合作模式'; cancel = '取消'; close = '關閉'; copylog = '複製記錄'; openfolder = '開啟遊戲資料夾'
  nogame = '找不到遊戲。請選擇包含 rerev.exe 的資料夾。'; noexe = '此資料夾中沒有 rerev.exe。'; found = '已找到 Resident Evil Revelations'
  badexe = '此 rerev.exe 不是支援的 Steam 版本 13298487。不會安裝任何內容。'
  have = '已安裝合作模式 {0}；此安裝程式為 {1}。'; same = '合作模式 {0} 已安裝。'
  what = '安裝程式會把模組複製到遊戲資料夾，並把被取代的檔案保存在 coop-backup 中。遊戲存檔和設定不會被變更。'
  need = '一台電腦上兩名玩家需要兩個控制器。在主選單按 F6 或 BACK 開啟合作大廳。'
  s_check = '正在檢查遊戲資料夾'; s_copy = '正在複製模組'; s_finish = '正在寫入安裝記錄和捷徑'; s_rm = '正在刪除模組檔案'; s_restore = '正在還原遊戲原有檔案'; s_clean = '正在刪除備份和模組資料夾'
  installing = '正在安裝合作模式 {0}'; updated = '已從 {0} 更新到 {1}'; installed = '合作模式 {0} 已安裝'; removing = '正在移除合作模式'; removed = '合作模式已移除'; ifail = '安裝失敗'; rfail = '移除失敗'
  done = '請透過「Resident Evil Revelations Co-op」捷徑（桌面、開始功能表）啟動遊戲：如有較新的版本，它會先從 GitHub 安裝。||在主選單按 F6 或 BACK 鍵開啟合作大廳。'
  donenolink = '像平常一樣啟動遊戲。在主選單按 F6 或 BACK 鍵開啟合作大廳。'
  gone = '模組已移除，被取代的檔案已還原。'; login = '記錄位於 {0}'; closegame = '請先關閉遊戲。'
  another = '另一個模組安裝正在進行：請等它結束。'; noadmin = '沒有系統管理員權限，Windows 不允許寫入遊戲資料夾。'
  incomplete = '下載的模組不完整：未做任何變更。請重新啟動遊戲再試一次。'; wait = '請等待完成。'
  pick = '請選擇 Resident Evil Revelations 資料夾'; confirm = '從 {0} 移除合作模組？'; kept = '已保留原檔案：'; copied = '已複製 {0} 個檔案。'; nrem = '已刪除 {0} 個檔案。'; putback = '已還原：'
  lnk = '捷徑：'; lnkdesc = '帶合作模組的 Resident Evil Revelations（如有較新的版本，會先安裝）'; err = '錯誤：'
  notmod = '{0} 中未安裝合作模組'; notgame = '不是 Resident Evil Revelations 資料夾：{0}'; nofiles = '安裝程式旁邊缺少「files」資料夾。'
  l_found = '合作模式 {0} 已發布（您目前為 {1}）。正在下載...'; l_verify = '正在驗證下載...'; l_failed = '無法安裝更新：{0}||遊戲將以目前版本（{1}）啟動。'; l_bad = '下載的檔案與其checksum不符。'
 }
}
