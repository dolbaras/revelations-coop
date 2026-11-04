/*
 * Texts of the Revelations 1 co-op lobby: own strings in the 13 languages of the game. Column order of every row (lobby1.h): English, French, Italian, German, Spanish,
 * Japanese, Russian, Polish, Dutch, Portuguese (Portugal), Portuguese (Brazil), Chinese (Simplified), Chinese (Traditional). A NULL column falls back to English.
 * Words of the stock menus that the sentences quote ($1 Continue, $2 Load Game, $3 New Game, $4 Campaign) are replaced at run time by the game's own strings of the running language
 * (menu.c reads them from Title_<lang>.gmd); the per-language fallbacks below are used when that file cannot be read.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "lobby1.h"

const char *const l1_lang_codes[L1_LANGS] = {"eng", "fre", "ita", "ger", "spa", "jpn", "rus", "pol", "dut", "por", "ptB", "chS", "chT"};
const char *const l1_lang_names[L1_LANGS] = {"English", "French", "Italian", "German", "Spanish", "Japanese", "Russian", "Polish", "Dutch", "Portuguese (Portugal)", "Portuguese (Brazil)",
                                             "Chinese (Simplified)", "Chinese (Traditional)"};

int l1_lang_from_langid(unsigned id)
{
    switch (id & 0x3FF) {   /* primary language */
    case 0x09: return 0;                                              /* English */
    case 0x0C: return 1;                                              /* French */
    case 0x10: return 2;                                              /* Italian */
    case 0x07: return 3;                                              /* German */
    case 0x0A: return 4;                                              /* Spanish */
    case 0x11: return 5;                                              /* Japanese */
    case 0x19: return 6;                                              /* Russian */
    case 0x15: return 7;                                              /* Polish */
    case 0x13: return 8;                                              /* Dutch */
    case 0x16: return (id >> 10) == 2 ? 9 : 10;                       /* Portuguese: sublanguage 2 = Portugal, 1 = Brazil */
    case 0x04: return (id == 0x0804 || id == 0x1004) ? 11 : 12;       /* Chinese: PRC and Singapore = simplified, the others traditional */
    default: return -1;
    }
}

int l1_lang_from_code(const char *code)
{
    if (!code) return -1;
    for (int i = 0; i < L1_LANGS; i++)
        if (!strcmp(code, l1_lang_codes[i])) return i;
    return -1;
}

int l1_lang_from_steam(const char *n)
{
    static const struct { const char *name; int idx; } map[] = {{"english", 0}, {"french", 1}, {"italian", 2}, {"german", 3}, {"spanish", 4}, {"japanese", 5}, {"russian", 6}, {"polish", 7},
                                                                 {"dutch", 8}, {"portuguese", 9}, {"brazilian", 10}, {"schinese", 11}, {"tchinese", 12}};
    if (!n) return -1;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(n, map[i].name)) return map[i].idx;
    return -1;
}

#define NL NULL
static const char *const T[L1T_COUNT][L1_LANGS] = {
    [L1T_TITLE] = {"Co-Op Play", "Jeu coopératif", "Gioco cooperativo", "Koop-Spiel", "Juego cooperativo", "協力プレイ", "Совместная игра", "Gra kooperacyjna", "Samen spelen",
                   "Jogo cooperativo", "Jogo cooperativo", "协力游玩", "協力遊玩"},
    [L1T_SUBTITLE] = {"Two players, one PC: you play the story together on a split screen. The second player takes over the partner character.",
                      "Deux joueurs, un seul PC : vous jouez l'histoire ensemble en écran partagé. Le deuxième joueur prend le contrôle du partenaire.",
                      "Due giocatori, un solo PC: giocate la storia insieme in split screen. Il secondo giocatore controlla il compagno.",
                      "Zwei Spieler, ein PC: Sie spielen die Geschichte gemeinsam im geteilten Bildschirm. Der zweite Spieler übernimmt den Partner.",
                      "Dos jugadores, un solo PC: jugáis la historia juntos en pantalla dividida. El segundo jugador controla al compañero.",
                      "2人で1台のPC：画面を分割して一緒にストーリーを遊びます。2人目はパートナーを操作します。",
                      "Два игрока, один ПК: вы проходите сюжет вместе на разделённом экране. Второй игрок управляет напарником.",
                      "Dwóch graczy, jeden komputer: przechodzicie fabułę razem na podzielonym ekranie. Drugi gracz przejmuje partnera.",
                      "Twee spelers, één pc: jullie spelen het verhaal samen op een gesplitst scherm. De tweede speler bestuurt de partner.",
                      "Dois jogadores, um PC: jogam a história juntos em ecrã dividido. O segundo jogador controla o parceiro.",
                      "Dois jogadores, um PC: vocês jogam a história juntos em tela dividida. O segundo jogador controla o parceiro.",
                      "两名玩家，一台电脑：以分屏方式一起游玩剧情。第二名玩家操控搭档角色。",
                      "兩名玩家，一台電腦：以分割畫面一起遊玩劇情。第二名玩家操控搭檔角色。"},
    [L1T_COOP] = {"Co-Op", "Coop", "Co-op", "Koop", "Cooperativo", "協力プレイ", "Кооператив", "Tryb kooperacji", "Co-op", "Cooperativo", "Cooperativo", "协力", "協力"},
    [L1T_ON] = {"On", "Activé", "Attivo", "An", "Activado", "オン", "Вкл.", "Włączone", "Aan", "Ligado", "Ligado", "开", "開"},
    [L1T_OFF] = {"Off", "Désactivé", "Disattivo", "Aus", "Desactivado", "オフ", "Выкл.", "Wyłączone", "Uit", "Desligado", "Desligado", "关", "關"},
    [L1T_P1PLAYS] = {"Player 1 plays", "Joueur 1 incarne", "Il giocatore 1 usa", "Spieler 1 spielt", "El jugador 1 controla a", "プレイヤー1の操作キャラ", "Игрок 1 играет за",
                     "Gracz 1 gra jako", "Speler 1 speelt", "O jogador 1 joga com", "O jogador 1 joga com", "玩家1操控", "玩家1操控"},
    [L1T_P2PLAYS] = {"Player 2 plays", "Joueur 2 incarne", "Il giocatore 2 usa", "Spieler 2 spielt", "El jugador 2 controla a", "プレイヤー2の操作キャラ", "Игрок 2 играет за",
                     "Gracz 2 gra jako", "Speler 2 speelt", "O jogador 2 joga com", "O jogador 2 joga com", "玩家2操控", "玩家2操控"},
    [L1T_LEAD] = {"lead character", "personnage principal", "personaggio principale", "Hauptfigur", "personaje principal", "主役キャラ", "основной персонаж", "główna postać",
                  "hoofdpersoon", "personagem principal", "personagem principal", "主角", "主角"},
    [L1T_PARTNER] = {"partner", "partenaire", "compagno", "Partner", "compañero", "パートナー", "напарник", "partner", "partner", "parceiro", "parceiro", "搭档", "搭檔"},
    [L1T_LAYOUT] = {"Screen layout", "Disposition de l'écran", "Disposizione dello schermo", "Bildschirmaufteilung", "Disposición de pantalla", "画面分割", "Разделение экрана",
                    "Podział ekranu", "Schermindeling", "Disposição do ecrã", "Divisão da tela", "分屏方式", "分割方式"},
    [L1T_SIDE] = {"Side by side", "Côte à côte", "Affiancato", "Nebeneinander", "Lado a lado", "左右に分割", "Слева и справа", "Obok siebie", "Naast elkaar", "Lado a lado", "Lado a lado",
                  "左右分屏", "左右分割"},
    [L1T_TOPBOTTOM] = {"Top and bottom", "Haut et bas", "Sopra e sotto", "Übereinander", "Arriba y abajo", "上下に分割", "Сверху и снизу", "Góra i dół", "Boven en onder",
                       "Em cima e em baixo", "Em cima e embaixo", "上下分屏", "上下分割"},
    [L1T_PADS] = {"Gamepads", "Manettes", "Controller", "Gamepads", "Mandos", "ゲームパッド", "Геймпады", "Gamepady", "Gamepads", "Comandos", "Controles", "手柄", "手把"},
    [L1T_PADS_NORMAL] = {"Player 1: gamepad 1, player 2: gamepad 2", "Joueur 1 : manette 1, joueur 2 : manette 2", "Giocatore 1: controller 1, giocatore 2: controller 2",
                         "Spieler 1: Gamepad 1, Spieler 2: Gamepad 2", "Jugador 1: mando 1, jugador 2: mando 2", "プレイヤー1：パッド1、プレイヤー2：パッド2",
                         "Игрок 1: геймпад 1, игрок 2: геймпад 2", "Gracz 1: gamepad 1, gracz 2: gamepad 2", "Speler 1: gamepad 1, speler 2: gamepad 2",
                         "Jogador 1: comando 1, jogador 2: comando 2", "Jogador 1: controle 1, jogador 2: controle 2", "玩家1：手柄1，玩家2：手柄2", "玩家1：手把1，玩家2：手把2"},
    [L1T_PADS_SWAPPED] = {"Player 1: gamepad 2, player 2: gamepad 1", "Joueur 1 : manette 2, joueur 2 : manette 1", "Giocatore 1: controller 2, giocatore 2: controller 1",
                          "Spieler 1: Gamepad 2, Spieler 2: Gamepad 1", "Jugador 1: mando 2, jugador 2: mando 1", "プレイヤー1：パッド2、プレイヤー2：パッド1",
                          "Игрок 1: геймпад 2, игрок 2: геймпад 1", "Gracz 1: gamepad 2, gracz 2: gamepad 1", "Speler 1: gamepad 2, speler 2: gamepad 1",
                          "Jogador 1: comando 2, jogador 2: comando 1", "Jogador 1: controle 2, jogador 2: controle 1", "玩家1：手柄2，玩家2：手柄1", "玩家1：手把2，玩家2：手把1"},
    [L1T_PADS_FOUND] = {"Gamepads connected", "Manettes connectées", "Controller collegati", "Gamepads verbunden", "Mandos conectados", "接続中のゲームパッド", "Подключено геймпадов",
                        "Podłączone gamepady", "Aangesloten gamepads", "Comandos ligados", "Controles conectados", "已连接的手柄", "已連接的手把"},
    [L1T_DONE] = {"Done", "Terminé", "Fatto", "Fertig", "Listo", "完了", "Готово", "Gotowe", "Klaar", "Concluído", "Concluir", "完成", "完成"},
    [L1T_HINT] = {"Up/Down: choose   Left/Right: change   Enter or A: confirm   Esc or B: back",
                  "Haut/Bas : choisir   Gauche/Droite : modifier   Entrée ou A : valider   Échap ou B : retour",
                  "Su/Giù: scegli   Sinistra/Destra: cambia   Invio o A: conferma   Esc o B: indietro",
                  "Hoch/Runter: wählen   Links/Rechts: ändern   Eingabe oder A: bestätigen   Esc oder B: zurück",
                  "Arriba/Abajo: elegir   Izq./Der.: cambiar   Intro o A: confirmar   Esc o B: atrás",
                  "上下: 選択   左右: 変更   Enter / A: 決定   Esc / B: 戻る",
                  "Вверх/вниз: выбор   Влево/вправо: изменить   Ввод или A: подтвердить   Esc или B: назад",
                  "Góra/Dół: wybór   Lewo/Prawo: zmiana   Enter lub A: zatwierdź   Esc lub B: wstecz",
                  "Omhoog/Omlaag: kiezen   Links/Rechts: wijzigen   Enter of A: bevestigen   Esc of B: terug",
                  "Cima/Baixo: escolher   Esq./Dir.: alterar   Enter ou A: confirmar   Esc ou B: voltar",
                  "Cima/Baixo: escolher   Esq./Dir.: alterar   Enter ou A: confirmar   Esc ou B: voltar",
                  "上/下：选择   左/右：更改   Enter 或 A：确认   Esc 或 B：返回",
                  "上/下：選擇   左/右：變更   Enter 或 A：確認   Esc 或 B：返回"},
    [L1T_ST_ON] = {"Co-op is on. Choose $1, $2 or $3 in the $4 menu to start playing together.",
                   "Le mode coopératif est activé. Choisissez $1, $2 ou $3 dans le menu $4 pour jouer ensemble.",
                   "La modalità cooperativa è attiva. Scegli $1, $2 o $3 nel menu $4 per giocare insieme.",
                   "Koop ist aktiv. Wählen Sie im Menü $4 $1, $2 oder $3, um gemeinsam zu spielen.",
                   "El modo cooperativo está activado. Elige $1, $2 o $3 en el menú $4 para jugar juntos.",
                   "協力プレイはオンです。$4メニューで「$1」「$2」「$3」を選ぶと、2人で遊べます。",
                   "Кооператив включён. Выберите «$1», «$2» или «$3» в меню «$4», чтобы играть вместе.",
                   "Tryb kooperacji jest włączony. Wybierz $1, $2 lub $3 w menu $4, aby grać razem.",
                   "Co-op staat aan. Kies $1, $2 of $3 in het menu $4 om samen te spelen.",
                   "O modo cooperativo está ligado. Escolha $1, $2 ou $3 no menu $4 para jogarem juntos.",
                   "O modo cooperativo está ligado. Escolha $1, $2 ou $3 no menu $4 para jogarem juntos.",
                   "协力模式已开启。请在“$4”菜单中选择“$1”“$2”或“$3”，即可一起游玩。",
                   "協力模式已開啟。請在「$4」選單中選擇「$1」「$2」或「$3」，即可一起遊玩。"},
    [L1T_ST_OFF] = {"Co-op is off. The game plays as usual.", "Le mode coopératif est désactivé. Le jeu se déroule normalement.", "La modalità cooperativa è disattivata. Il gioco si svolge normalmente.",
                    "Koop ist aus. Das Spiel läuft wie gewohnt.", "El modo cooperativo está desactivado. El juego funciona como siempre.", "協力プレイはオフです。通常どおり遊べます。",
                    "Кооператив выключен. Игра идёт как обычно.", "Tryb kooperacji jest wyłączony. Gra działa jak zwykle.", "Co-op staat uit. Het spel werkt zoals gewoonlijk.",
                    "O modo cooperativo está desligado. O jogo decorre normalmente.", "O modo cooperativo está desligado. O jogo funciona normalmente.", "协力模式已关闭，游戏照常进行。",
                    "協力模式已關閉，遊戲照常進行。"},
    [L1T_WARN_NOPAD] = {"No gamepad found: connect two gamepads to play together.", "Aucune manette détectée : branchez deux manettes pour jouer ensemble.",
                        "Nessun controller trovato: collega due controller per giocare insieme.", "Kein Gamepad gefunden: Schließen Sie zwei Gamepads an, um gemeinsam zu spielen.",
                        "No se ha encontrado ningún mando: conecta dos mandos para jugar juntos.", "ゲームパッドが見つかりません。2人で遊ぶには2台接続してください。",
                        "Геймпады не найдены: подключите два геймпада, чтобы играть вместе.", "Nie znaleziono gamepada: podłącz dwa gamepady, aby grać razem.",
                        "Geen gamepad gevonden: sluit twee gamepads aan om samen te spelen.", "Nenhum comando encontrado: ligue dois comandos para jogarem juntos.",
                        "Nenhum controle encontrado: conecte dois controles para jogarem juntos.", "未找到手柄：请连接两个手柄以一起游玩。", "未找到手把：請連接兩個手把以一起遊玩。"},
    [L1T_WARN_ONEPAD] = {"One gamepad found: connect a second gamepad for player 2.", "Une seule manette détectée : branchez une deuxième manette pour le joueur 2.",
                         "Trovato un solo controller: collega un secondo controller per il giocatore 2.", "Nur ein Gamepad gefunden: Schließen Sie für Spieler 2 ein zweites Gamepad an.",
                         "Solo se ha encontrado un mando: conecta un segundo mando para el jugador 2.", "ゲームパッドが1台のみです。プレイヤー2用にもう1台接続してください。",
                         "Найден только один геймпад: подключите второй для игрока 2.", "Znaleziono tylko jeden gamepad: podłącz drugi dla gracza 2.",
                         "Slechts één gamepad gevonden: sluit een tweede gamepad aan voor speler 2.", "Só foi encontrado um comando: ligue um segundo comando para o jogador 2.",
                         "Só foi encontrado um controle: conecte um segundo controle para o jogador 2.", "只找到一个手柄：请为玩家2连接第二个手柄。", "只找到一個手把：請為玩家2連接第二個手把。"},
    [L1T_PLAYER1] = {"PLAYER 1", "JOUEUR 1", "GIOCATORE 1", "SPIELER 1", "JUGADOR 1", "プレイヤー1", "ИГРОК 1", "GRACZ 1", "SPELER 1", "JOGADOR 1", "JOGADOR 1", "玩家1", "玩家1"},
    [L1T_PLAYER2] = {"PLAYER 2", "JOUEUR 2", "GIOCATORE 2", "SPIELER 2", "JUGADOR 2", "プレイヤー2", "ИГРОК 2", "GRACZ 2", "SPELER 2", "JOGADOR 2", "JOGADOR 2", "玩家2", "玩家2"},
    [L1T_PAD1] = {"Gamepad 1", "Manette 1", "Controller 1", "Gamepad 1", "Mando 1", "ゲームパッド1", "Геймпад 1", "Gamepad 1", "Gamepad 1", "Comando 1", "Controle 1", "手柄1", "手把1"},
    [L1T_PAD2] = {"Gamepad 2", "Manette 2", "Controller 2", "Gamepad 2", "Mando 2", "ゲームパッド2", "Геймпад 2", "Gamepad 2", "Gamepad 2", "Comando 2", "Controle 2", "手柄2", "手把2"},
    [L1T_NOTE_SAVE] = {"Co-op plays the normal story: it uses your usual save slot and checkpoints.",
                       "Le mode coopératif joue l'histoire normale : il utilise votre emplacement de sauvegarde et vos points de contrôle habituels.",
                       "La cooperativa gioca la storia normale: usa il tuo slot di salvataggio e i checkpoint di sempre.",
                       "Koop spielt die normale Geschichte: Es nutzt Ihren üblichen Speicherstand und die Checkpoints.",
                       "El modo cooperativo juega la historia normal: usa tu ranura de guardado y tus puntos de control de siempre.",
                       "協力プレイは通常のストーリーを遊びます。いつものセーブスロットとチェックポイントを使用します。",
                       "Кооператив проходит обычный сюжет: используются ваш слот сохранения и контрольные точки.",
                       "Tryb kooperacji przechodzi zwykłą fabułę: korzysta z twojego slotu zapisu i punktów kontrolnych.",
                       "Co-op speelt het gewone verhaal: je gebruikt je gebruikelijke opslagslot en checkpoints.",
                       "O modo cooperativo joga a história normal: usa o seu espaço de gravação e os pontos de controlo habituais.",
                       "O modo cooperativo joga a história normal: usa seu espaço de salvamento e os checkpoints de sempre.",
                       "协力模式游玩的是正常剧情：沿用你原来的存档位和检查点。", "協力模式遊玩的是正常劇情：沿用你原本的存檔位和檢查點。"},
    [L1T_NOTE_SOLO] = {"In a part of the story with only one playable character, the second player waits.",
                       "Dans une partie de l'histoire avec un seul personnage jouable, le deuxième joueur attend.",
                       "In una parte della storia con un solo personaggio giocabile, il secondo giocatore attende.",
                       "In einem Abschnitt der Geschichte mit nur einer spielbaren Figur wartet der zweite Spieler.",
                       "En una parte de la historia con un solo personaje jugable, el segundo jugador espera.",
                       "操作できるキャラが1人だけの場面では、2人目は待機します。",
                       "В части сюжета с одним играбельным персонажем второй игрок ждёт.",
                       "W części fabuły z jedną grywalną postacią drugi gracz czeka.",
                       "In een deel van het verhaal met maar één speelbaar personage wacht de tweede speler.",
                       "Numa parte da história com apenas uma personagem jogável, o segundo jogador espera.",
                       "Numa parte da história com apenas um personagem jogável, o segundo jogador espera.",
                       "在只有一名可操控角色的剧情段落中，第二名玩家需要等待。", "在只有一名可操控角色的劇情段落中，第二名玩家需要等待。"},
    [L1T_HINT_TITLE] = {"F6 / Back button: Co-Op Play", "F6 / bouton Retour : jeu coopératif", "F6 / tasto Indietro: gioco cooperativo", "F6 / Zurück-Taste: Koop-Spiel",
                        "F6 / botón Atrás: juego cooperativo", "F6 / BACKボタン：協力プレイ", "F6 / кнопка Back: совместная игра", "F6 / przycisk Back: gra kooperacyjna",
                        "F6 / Back-knop: samen spelen", "F6 / botão Back: jogo cooperativo", "F6 / botão Back: jogo cooperativo", "F6 / Back键：协力游玩", "F6 / Back鍵：協力遊玩"},
    [L1T_BANNER_ON] = {"Co-Op is ON", "Coop ACTIVÉ", "Co-op ATTIVA", "Koop AN", "Cooperativo ACTIVADO", "協力プレイ オン", "Кооператив ВКЛ.", "Kooperacja WŁ.", "Co-op AAN",
                       "Cooperativo LIGADO", "Cooperativo LIGADO", "协力已开启", "協力已開啟"},
    [L1T_BANNER_OFF] = {"Co-Op is off", "Coop désactivé", "Co-op disattivata", "Koop aus", "Cooperativo desactivado", "協力プレイ オフ", "Кооператив выкл.", "Kooperacja wył.", "Co-op uit",
                        "Cooperativo desligado", "Cooperativo desligado", "协力已关闭", "協力已關閉"},
    [L1T_NEXT_SECTION] = {"The characters shown are those of Episode 1. Other parts of the story have other pairs.",
                          "Les personnages affichés sont ceux de l'épisode 1. Les autres parties de l'histoire ont d'autres duos.",
                          "I personaggi mostrati sono quelli dell'Episodio 1. Altre parti della storia hanno altre coppie.",
                          "Die gezeigten Figuren gehören zu Episode 1. Andere Teile der Geschichte haben andere Paare.",
                          "Los personajes mostrados son los del Episodio 1. Otras partes de la historia tienen otras parejas.",
                          "表示されているのはエピソード1のキャラクターです。他の場面では組み合わせが変わります。",
                          "Показаны персонажи 1-го эпизода. В других частях сюжета другие пары.",
                          "Pokazane postacie pochodzą z epizodu 1. Inne części fabuły mają inne pary.",
                          "De getoonde personages horen bij aflevering 1. Andere delen van het verhaal hebben andere duo's.",
                          "As personagens mostradas são as do Episódio 1. Outras partes da história têm outros pares.",
                          "Os personagens mostrados são os do Episódio 1. Outras partes da história têm outras duplas.",
                          "所示角色为第1章的角色，剧情其他部分会有不同的搭档组合。", "所示角色為第1章的角色，劇情其他部分會有不同的搭檔組合。"},
    [L1T_RAID_NOTE] = {"Raid Mode and online play are never touched by this mod.", "Le mode Raid et le jeu en ligne ne sont jamais modifiés par ce mod.",
                       "La modalità Raid e il gioco online non vengono mai toccati da questa mod.", "Der Raid-Modus und Online-Spiele werden von dieser Mod nie verändert.",
                       "El modo Raid y el juego en línea nunca se modifican con este mod.", "レイドモードとオンラインプレイには一切干渉しません。",
                       "Режим «Рейд» и онлайн-игра никогда не затрагиваются этим модом.", "Ten mod nigdy nie ingeruje w tryb Raid ani grę online.",
                       "Raid-modus en online spelen worden door deze mod nooit aangeraakt.", "O modo Raid e o jogo online nunca são alterados por este mod.",
                       "O modo Raid e o jogo online nunca são alterados por este mod.", "本模组绝不会改动突袭模式和在线游戏。", "本模組絕不會改動突襲模式和線上遊戲。"},
};

const char *l1_text(int id, int lang)
{
    if (id < 0 || id >= L1T_COUNT) return "";
    if (lang < 0 || lang >= L1_LANGS) lang = 0;
    const char *s = T[id][lang];
    if (!s) s = T[id][0];
    return s ? s : "";
}

/* ---- stock words quoted by the sentences ---- */
static const char *const WORDS[4][L1_LANGS] = {
    /* $1 Continue */ {"Continue", "Continuer", "Continua", "Fortsetzen", "Continuar", "コンティニュー", "Продолжить", "Kontynuuj", "Doorgaan", "Continuar", "Continuar", "继续", "繼續"},
    /* $2 Load Game */ {"Load Game", "Charger partie", "Carica partita", "Spiel laden", "Cargar partida", "ロード", "Загрузить игру", "Wczytaj grę", "Spel laden", "Carregar jogo", "Carregar jogo",
                        "读取游戏", "讀取遊戲"},
    /* $3 New Game */ {"New Game", "Nouvelle partie", "Nuova partita", "Neues Spiel", "Nueva partida", "ニューゲーム", "Новая игра", "Nowa gra", "Nieuw spel", "Novo jogo", "Novo jogo", "新游戏", "新遊戲"},
    /* $4 Campaign */ {"Campaign", "Campagne", "Campagna", "Kampagne", "Campaña", "キャンペーン", "Кампания", "Kampania", "Campagne", "Campanha", "Campanha", "战役", "戰役"},
};
static char g_words[4][64];

void l1_set_stock_words(const char words[4][64])
{
    for (int i = 0; i < 4; i++) {
        if (words && words[i][0]) snprintf(g_words[i], sizeof g_words[i], "%s", words[i]);
        else g_words[i][0] = 0;
    }
}

void l1_expand(const char *tmpl, int lang, char *out, size_t size)
{
    size_t o = 0;
    if (!size) return;
    if (lang < 0 || lang >= L1_LANGS) lang = 0;
    for (const char *p = tmpl; *p && o + 1 < size; p++) {
        if (p[0] == '$' && p[1] >= '1' && p[1] <= '4') {
            int k = p[1] - '1';
            const char *w = g_words[k][0] ? g_words[k] : WORDS[k][lang];
            if (!w) w = WORDS[k][0];
            while (*w && o + 1 < size) out[o++] = *w++;
            p++;
        } else
            out[o++] = *p;
    }
    out[o] = 0;
}
