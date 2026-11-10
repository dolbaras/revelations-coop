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
    /* the game's own rule (build 13298487, 0x9BC7B5: GetUserDefaultLangID -> sMain.mLanguage, then 0x844010 picks the text language): the primary language decides, except
       Chinese (only 0x0404 Taiwan is traditional: Hong Kong, Macau, PRC, Singapore read the simplified texts) and Portuguese (only 0x0416 is Brazilian) */
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
    case 0x16: return id == 0x0416 ? 10 : 9;                          /* Portuguese: Brazil, else Portugal */
    case 0x04: return id == 0x0404 ? 12 : 11;                         /* Chinese: Taiwan traditional, else simplified */
    default: return -1;                                               /* the game shows English (Korean, Ukrainian, Finnish ... included) */
    }
}

int l1_lang_from_game(unsigned m)
{
    /* sMain.mLanguage (the game's language index; its file suffix table at 0x115C958): 0 jpn 1 eng 2 fre 3 spa 4 ger 5 ita 6 kor 7 chT 8 chS 9 por 10 rus 11 pol 12 dut 13 fin 14 swe
       15 dan 16 nor 17 cze 18 hun 19 slo 20 oth 21 ptB. The game ships 13 of them; for kor / fin ... oth its text language is English (0x844010) */
    static const signed char map[22] = {5, 0, 1, 4, 3, 2, 0, 12, 11, 9, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 10};
    return m < 22 ? map[m] : -1;
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
    /* rev1 live 16 (the lead's decision: split-screen co-op also uses the co-op save): the split view's save line */
    [L1T_NOTE_SAVE] = {"Co-op plays the normal story with its own co-op save: your solo campaign save is not changed.",
                       "Le mode coopératif joue l'histoire normale avec sa propre sauvegarde coop : votre sauvegarde de campagne solo n'est pas modifiée.",
                       "La cooperativa gioca la storia normale con un proprio salvataggio co-op: il tuo salvataggio della campagna in solitario non cambia.",
                       "Koop spielt die normale Geschichte mit einem eigenen Koop-Spielstand: Ihr Solo-Kampagnen-Spielstand bleibt unverändert.",
                       "El modo cooperativo juega la historia normal con su propia partida cooperativa: tu partida de la campaña en solitario no cambia.",
                       "協力プレイは専用のセーブデータで通常のストーリーを遊びます。ソロのキャンペーンのセーブデータは変わりません。",
                       "Кооператив проходит обычный сюжет со своим кооперативным сохранением: ваше сохранение одиночной кампании не меняется.",
                       "Tryb kooperacji przechodzi zwykłą fabułę z własnym zapisem kooperacji: twój zapis kampanii solo się nie zmienia.",
                       "Co-op speelt het gewone verhaal met een eigen co-opopslag: je solo-campagneopslag blijft ongewijzigd.",
                       "O modo cooperativo joga a história normal com a sua própria gravação cooperativa: a sua gravação da campanha a solo não é alterada.",
                       "O modo cooperativo joga a história normal com seu próprio save cooperativo: seu save da campanha solo não é alterado.",
                       "协力模式使用独立的协力存档游玩正常剧情：你的单人战役存档不会改变。", "協力模式使用獨立的協力存檔遊玩正常劇情：你的單人戰役存檔不會改變。"},
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
    /* (in capitals where the game's own rows of that language are: Title_<lang>.gmd menu_campaign "КАМПАНИЯ", "KAMPANIA", "CAMPAGNE", "CAMPANHA") */
    [L1T_ROW_LABEL] = {"Co-Op Play", "Jeu coopératif", "Gioco cooperativo", "Koop-Spiel", "Juego cooperativo", "協力プレイ", "СОВМЕСТНАЯ ИГРА", "GRA KOOPERACYJNA", "CO-OP SPELEN",
                       "JOGO COOPERATIVO", "JOGO COOPERATIVO", "协力游玩", "協力遊玩"},
    [L1T_ROW_HELP] = {"Two players: split screen on one PC, or over the network.", "Deux joueurs : écran partagé sur un PC, ou en réseau.", "Due giocatori: schermo diviso su un PC o in rete.",
                      "Zwei Spieler: geteilter Bildschirm an einem PC oder über das Netzwerk.", "Dos jugadores: pantalla dividida en un PC o por red.", "2人プレイ：1台のPCで画面分割、またはネットワーク経由。",
                      "Два игрока: разделённый экран на одном ПК или по сети.", "Dwóch graczy: podzielony ekran na jednym komputerze albo przez sieć.", "Twee spelers: gesplitst scherm op één pc, of via het netwerk.",
                      "Dois jogadores: ecrã dividido num PC, ou em rede.", "Dois jogadores: tela dividida em um PC, ou pela rede.", "两名玩家：一台电脑分屏，或通过网络。", "兩名玩家：一台電腦分割畫面，或透過網路。"},
    [L1T_LEG_OK] = {"OK", "OK", "OK", "OK", "OK", "決定", "OK", "OK", "OK", "OK", "OK", "确定", "確定"},
    [L1T_LEG_BACK] = {"Back", "Retour", "Indietro", "Zurück", "Atrás", "戻る", "Назад", "Wstecz", "Terug", "Voltar", "Voltar", "返回", "返回"},
    /* network rows (lobby1.h: env.net_rows) */
    [L1T_NET] = {"Network", "Réseau", "Rete", "Netzwerk", "Red", "ネットワーク", "Сеть", "Sieć", "Netwerk", "Rede", "Rede", "网络", "網路"},
    [L1T_NET_HOST] = {"Host", "Héberger", "Ospita", "Hosten", "Alojar", "ホスト", "Хост", "Hostuj", "Hosten", "Hospedar", "Hospedar", "主机", "主機"},
    [L1T_NET_JOIN] = {"Join", "Rejoindre", "Unisciti", "Beitreten", "Unirse", "参加", "Подключиться", "Dołącz", "Deelnemen", "Entrar", "Entrar", "加入", "加入"},
    [L1T_NET_ADDR] = {"Host address", "Adresse de l'hôte", "Indirizzo host", "Adresse des Hosts", "Dirección del anfitrión", "ホストのアドレス", "Адрес хоста", "Adres hosta",
                      "Adres van de host", "Endereço do anfitrião", "Endereço do anfitrião", "主机地址", "主機位址"},
    [L1T_NET_OWN] = {"Your address", "Votre adresse", "Il tuo indirizzo", "Deine Adresse", "Tu dirección", "自分のアドレス", "Ваш адрес", "Twój adres", "Jouw adres",
                     "O seu endereço", "Seu endereço", "你的地址", "你的位址"},
    [L1T_NET_ADDR_HINT] = {"type IP:port", "saisissez IP:port", "digita IP:porta", "IP:Port eingeben", "escribe IP:puerto", "IP:ポートを入力", "введите IP:порт", "wpisz IP:port",
                           "typ IP:poort", "digite IP:porta", "digite IP:porta", "输入 IP:端口", "輸入 IP:埠"},
    [L1T_NET_START] = {"Start", "Démarrer", "Avvia", "Starten", "Iniciar", "開始", "Начать", "Uruchom", "Starten", "Iniciar", "Iniciar", "开始", "開始"},
    [L1T_NET_STOP] = {"Disconnect", "Déconnecter", "Disconnetti", "Trennen", "Desconectar", "切断", "Отключиться", "Rozłącz", "Verbreken", "Desconectar", "Desconectar", "断开", "斷開"},
    [L1T_NET_READY] = {"I am ready", "Je suis prêt", "Sono pronto", "Ich bin bereit", "Estoy listo", "準備完了", "Я готов", "Jestem gotowy", "Ik ben klaar", "Estou pronto",
                       "Estou pronto", "我已准备", "我已準備"},
    [L1T_NET_ST_OFF] = {"Network is off: this game runs on this PC only.", "Le réseau est désactivé : le jeu reste sur ce PC.", "La rete è disattivata: il gioco resta su questo PC.",
                        "Netzwerk aus: Das Spiel läuft nur auf diesem PC.", "La red está desactivada: el juego se queda en este PC.", "ネットワークはオフです。このPCだけで遊びます。",
                        "Сеть выключена: игра идёт только на этом ПК.", "Sieć wyłączona: gra działa tylko na tym komputerze.", "Netwerk uit: het spel draait alleen op deze pc.",
                        "A rede está desligada: o jogo fica neste PC.", "A rede está desligada: o jogo fica neste PC.", "网络已关闭：游戏只在这台电脑上运行。", "網路已關閉：遊戲只在這台電腦上運行。"},
    [L1T_NET_ST_HOST] = {"Press Start: the other player joins with your address.", "Appuyez sur Démarrer : l'autre joueur rejoint avec votre adresse.",
                         "Premi Avvia: l'altro giocatore si unisce con il tuo indirizzo.", "Starten drücken: Der andere Spieler tritt mit Ihrer Adresse bei.",
                         "Pulsa Iniciar: el otro jugador se une con tu dirección.", "「開始」を押すと、もう1人があなたのアドレスで参加できます。",
                         "Нажмите «Начать»: второй игрок подключится по вашему адресу.", "Naciśnij Start: drugi gracz dołączy po twoim adresie.",
                         "Druk op Start: de andere speler sluit aan met jouw adres.", "Carregue em Iniciar: o outro jogador entra com o seu endereço.",
                         "Carregue em Iniciar: o outro jogador entra com seu endereço.", "按“开始”，另一名玩家即可用你的地址加入。", "按「開始」，另一名玩家即可用你的位址加入。"},
    [L1T_NET_ST_WAIT] = {"Waiting for the other player to join...", "En attente de l'autre joueur...", "In attesa dell'altro giocatore...", "Warten auf den anderen Spieler...",
                         "Esperando al otro jugador...", "もう1人の参加を待っています…", "Ожидание второго игрока...", "Czekam na drugiego gracza...", "Wachten op de andere speler...",
                         "A aguardar o outro jogador...", "Aguardando o outro jogador...", "等待另一名玩家加入……", "等待另一名玩家加入……"},
    [L1T_NET_ST_JOIN] = {"Press Start to connect to the host.", "Appuyez sur Démarrer pour rejoindre l'hôte.", "Premi Avvia per connetterti all'host.",
                         "Starten drücken, um sich mit dem Host zu verbinden.", "Pulsa Iniciar para conectar con el anfitrión.", "「開始」を押してホストに接続します。",
                         "Нажмите «Начать», чтобы подключиться к хосту.", "Naciśnij Start, aby połączyć się z hostem.", "Druk op Start om verbinding te maken met de host.",
                         "Carregue em Iniciar para ligar ao anfitrião.", "Carregue em Iniciar para ligar ao anfitrião.", "按“开始”连接到主机。", "按「開始」連線到主機。"},
    [L1T_NET_ST_BADADDR] = {"The address must look like 192.168.0.10:28410.", "L'adresse doit ressembler à 192.168.0.10:28410.", "L'indirizzo deve avere la forma 192.168.0.10:28410.",
                            "Die Adresse muss wie 192.168.0.10:28410 aussehen.", "La dirección debe tener la forma 192.168.0.10:28410.", "アドレスは 192.168.0.10:28410 のような形式で入力してください。",
                            "Адрес должен быть вида 192.168.0.10:28410.", "Adres musi wyglądać tak: 192.168.0.10:28410.", "Het adres moet er zo uitzien: 192.168.0.10:28410.",
                            "O endereço deve ter a forma 192.168.0.10:28410.", "O endereço deve ter o formato 192.168.0.10:28410.", "地址格式应类似 192.168.0.10:28410。", "位址格式應類似 192.168.0.10:28410。"},
    [L1T_NET_ST_CONNECTING] = {"Connecting to the host...", "Connexion à l'hôte...", "Connessione all'host...", "Verbindung zum Host wird aufgebaut...", "Conectando con el anfitrión...",
                               "ホストに接続しています…", "Подключение к хосту...", "Łączenie z hostem...", "Verbinden met de host...", "A ligar ao anfitrião...", "Conectando ao anfitrião...",
                               "正在连接主机……", "正在連接主機……"},
    [L1T_NET_ST_CONN] = {"Connected. Mark yourself ready when you are set.", "Connecté. Indiquez que vous êtes prêt quand vous l'êtes.", "Connesso. Segnala di essere pronto quando sei pronto.",
                         "Verbunden. Markieren Sie sich als bereit, wenn es so weit ist.", "Conectado. Márcate como listo cuando lo estés.", "接続しました。準備ができたら「準備完了」にしてください。",
                         "Подключено. Отметьте готовность, когда будете готовы.", "Połączono. Zaznacz gotowość, gdy będziesz gotowy.", "Verbonden. Zet aan dat je klaar bent als je zover bent.",
                         "Ligado. Marque que está pronto quando estiver.", "Ligado. Marque que está pronto quando estiver.", "已连接。准备好后请选择“我已准备”。", "已連線。準備好後請選擇「我已準備」。"},
    [L1T_NET_ST_WAITREADY] = {"You are ready. Waiting for the other player...", "Vous êtes prêt. En attente de l'autre joueur...", "Sei pronto. In attesa dell'altro giocatore...",
                              "Sie sind bereit. Warten auf den anderen Spieler...", "Estás listo. Esperando al otro jugador...", "準備完了です。もう1人を待っています…",
                              "Вы готовы. Ожидание второго игрока...", "Jesteś gotowy. Czekam na drugiego gracza...", "Jij bent klaar. Wachten op de andere speler...",
                              "Está pronto. A aguardar o outro jogador...", "Está pronto. Aguardando o outro jogador...", "你已准备，等待另一名玩家……", "你已準備，等待另一名玩家……"},
    [L1T_NET_ST_PEERREADY] = {"The other player is ready. Mark yourself ready to start.", "L'autre joueur est prêt. Indiquez que vous l'êtes aussi pour commencer.",
                              "L'altro giocatore è pronto. Segnala di essere pronto per iniziare.", "Der andere Spieler ist bereit. Markieren Sie sich ebenfalls als bereit.",
                              "El otro jugador está listo. Márcate como listo para empezar.", "もう1人は準備完了です。あなたも準備完了にしてください。",
                              "Второй игрок готов. Отметьте готовность, чтобы начать.", "Drugi gracz jest gotowy. Zaznacz gotowość, aby zacząć.",
                              "De andere speler is klaar. Zet jezelf ook op klaar om te beginnen.", "O outro jogador está pronto. Marque-se também como pronto para começar.",
                              "O outro jogador está pronto. Marque-se também como pronto para começar.", "另一名玩家已准备。请也选择“我已准备”开始游戏。", "另一名玩家已準備。請也選擇「我已準備」開始遊戲。"},
    [L1T_NET_ST_BOTH] = {"Both players are ready.", "Les deux joueurs sont prêts.", "Entrambi i giocatori sono pronti.", "Beide Spieler sind bereit.", "Ambos jugadores están listos.",
                         "2人とも準備完了です。", "Оба игрока готовы.", "Obaj gracze są gotowi.", "Beide spelers zijn klaar.", "Os dois jogadores estão prontos.",
                         "Os dois jogadores estão prontos.", "两名玩家都已准备。", "兩名玩家都已準備。"},
    [L1T_NET_ST_ERROR] = {"Connection failed. Check the address and the firewall, then press Start.", "Échec de la connexion. Vérifiez l'adresse et le pare-feu, puis appuyez sur Démarrer.",
                          "Connessione fallita. Controlla indirizzo e firewall, poi premi Avvia.", "Verbindung fehlgeschlagen. Prüfen Sie Adresse und Firewall und drücken Sie Starten.",
                          "La conexión falló. Revisa la dirección y el cortafuegos y pulsa Iniciar.", "接続に失敗しました。アドレスとファイアウォールを確認して「開始」を押してください。",
                          "Не удалось подключиться. Проверьте адрес и брандмауэр и нажмите «Начать».", "Połączenie nie powiodło się. Sprawdź adres i zaporę, potem naciśnij Start.",
                          "Verbinding mislukt. Controleer het adres en de firewall en druk op Start.", "A ligação falhou. Verifique o endereço e a firewall e carregue em Iniciar.",
                          "A ligação falhou. Verifique o endereço e o firewall e carregue em Iniciar.", "连接失败。请检查地址和防火墙，然后按“开始”。", "連線失敗。請檢查位址和防火牆，然後按「開始」。"},
    /* lobby-must: the exclusive Mode row, the network game, refusal reasons, in-game strips (%s = a name, an address or a version; the counts are checked by tests/test_lobby1.c) */
    [L1T_MODE_SPLIT] = {"Split screen", "Écran partagé", "Schermo diviso", "Geteilter Bildschirm", "Pantalla dividida", "画面分割", "Разделённый экран", "Podzielony ekran", "Gesplitst scherm",
                        "Ecrã dividido", "Tela dividida", "分屏", "分割畫面"},
    [L1T_MODE_HOST] = {"Network: host", "Réseau : héberger", "Rete: ospita", "Netzwerk: hosten", "Red: alojar", "ネットワーク：ホスト", "Сеть: хост", "Sieć: hostuj", "Netwerk: hosten",
                       "Rede: anfitrião", "Rede: hospedar", "网络：主机", "網路：主機"},
    [L1T_MODE_JOIN] = {"Network: join", "Réseau : rejoindre", "Rete: unisciti", "Netzwerk: beitreten", "Red: unirse", "ネットワーク：参加", "Сеть: подключиться", "Sieć: dołącz", "Netwerk: deelnemen",
                       "Rede: entrar", "Rede: entrar", "网络：加入", "網路：加入"},
    [L1T_SUBTITLE_NET] = {"Two players, two PCs: each plays one character over the network.", "Deux joueurs, deux PC : chacun joue un personnage en réseau.",
                          "Due giocatori, due PC: ognuno gioca un personaggio in rete.", "Zwei Spieler, zwei PCs: Jeder spielt eine Figur über das Netzwerk.",
                          "Dos jugadores, dos PC: cada uno controla un personaje por red.", "2人で2台のPC：ネットワーク経由でそれぞれ1人のキャラクターを操作します。",
                          "Два игрока, два ПК: каждый играет за своего персонажа по сети.", "Dwóch graczy, dwa komputery: każdy gra jedną postacią przez sieć.",
                          "Twee spelers, twee pc's: ieder speelt één personage via het netwerk.", "Dois jogadores, dois PCs: cada um joga com uma personagem em rede.",
                          "Dois jogadores, dois PCs: cada um joga com um personagem pela rede.", "两名玩家，两台电脑：通过网络各自操控一名角色。",
                          "兩名玩家，兩台電腦：透過網路各自操控一名角色。"},
    [L1T_YOU_PLAY] = {"You play", "Vous incarnez", "Giochi con", "Sie spielen", "Controlas a", "あなたの操作キャラ", "Вы играете за", "Grasz jako", "Jij speelt", "Joga com", "Você joga com", "你操控", "你操控"},
    [L1T_NET_PEER] = {"%s (network)", "%s (réseau)", "%s (rete)", "%s (Netzwerk)", "%s (red)", "%s（ネットワーク）", "%s (по сети)", "%s (sieć)", "%s (netwerk)", "%s (rede)", "%s (rede)", "%s（网络）",
                      "%s（網路）"},
    [L1T_JOINING_AS] = {"Joining as %s.", "Vous rejoignez la partie en tant que %s.", "Partecipi come %s.", "Sie treten bei als %s.", "Te unes como %s.", "%sとして参加します。", "Вы подключаетесь за %s.",
                        "Dołączasz jako %s.", "Je speelt mee als %s.", "Entra como %s.", "Você entra como %s.", "你将以%s的身份加入。", "你將以%s的身分加入。"},
    [L1T_NET_ST_CONNECTING_TO] = {"Connecting to %s...", "Connexion à %s...", "Connessione a %s...", "Verbindung zu %s wird aufgebaut...", "Conectando con %s...", "%sに接続しています…",
                                  "Подключение к %s...", "Łączenie z %s...", "Verbinden met %s...", "A ligar a %s...", "Conectando a %s...", "正在连接 %s……", "正在連線 %s……"},
    [L1T_NET_ST_MARK] = {"Mark yourself ready when you are set.", "Indiquez que vous êtes prêt quand vous l'êtes.", "Segnala di essere pronto quando sei pronto.",
                         "Markieren Sie sich als bereit, wenn es so weit ist.", "Márcate como listo cuando lo estés.", "準備ができたら「準備完了」にしてください。", "Отметьте готовность, когда будете готовы.",
                         "Zaznacz gotowość, gdy będziesz gotowy.", "Zet aan dat je klaar bent als je zover bent.", "Marque que está pronto quando estiver.", "Marque que está pronto quando estiver.",
                         "准备好后请选择“我已准备”。", "準備好後請選擇「我已準備」。"},
    [L1T_NET_ST_GO_HOST] = {"Start Episode 1: $4 > $1 or $3. %s follows you automatically.", "Lancez l'épisode 1 : $4 > $1 ou $3. %s vous suit automatiquement.",
                            "Avvia l'Episodio 1: $4 > $1 o $3. %s ti segue automaticamente.", "Starten Sie Episode 1: $4 > $1 oder $3. %s folgt Ihnen automatisch.",
                            "Empieza el Episodio 1: $4 > $1 o $3. %s te sigue automáticamente.", "エピソード1を開始してください：「$4」>「$1」または「$3」。%sは自動的に合流します。",
                            "Начните эпизод 1: «$4» > «$1» или «$3». %s подключится автоматически.", "Rozpocznij epizod 1: $4 > $1 lub $3. %s dołączy automatycznie.",
                            "Start aflevering 1: $4 > $1 of $3. %s volgt je vanzelf.", "Comece o Episódio 1: $4 > $1 ou $3. %s acompanha-o automaticamente.",
                            "Comece o Episódio 1: $4 > $1 ou $3. %s acompanha você automaticamente.", "请开始第1章：“$4” > “$1”或“$3”。%s会自动跟随你。", "請開始第1章：「$4」>「$1」或「$3」。%s會自動跟隨你。"},
    [L1T_NET_ST_GO_GUEST] = {"Waiting for %s to start the story. Your game follows automatically.", "En attente que %s lance l'histoire. Votre jeu suit automatiquement.",
                             "In attesa che %s avvii la storia. Il tuo gioco seguirà automaticamente.", "Warten, bis %s die Geschichte startet. Ihr Spiel folgt automatisch.",
                             "Esperando a que %s empiece la historia. Tu juego le seguirá automáticamente.", "%sがストーリーを開始するのを待っています。あなたのゲームは自動的に合流します。",
                             "Ожидание: %s начинает сюжет. Ваша игра подключится автоматически.", "Czekam, aż %s rozpocznie fabułę. Twoja gra dołączy automatycznie.",
                             "Wachten tot %s het verhaal start. Je spel volgt vanzelf.", "A aguardar que %s comece a história. O seu jogo acompanha automaticamente.",
                             "Aguardando %s começar a história. Seu jogo acompanha automaticamente.", "正在等待%s开始剧情，你的游戏会自动跟随。", "正在等待%s開始劇情，你的遊戲會自動跟隨。"},
    [L1T_NET_HOST_HINT] = {"Over the internet your friend needs your public address and UDP port 28410 forwarded to this PC; allow the game in Windows Firewall.",
                           "Par Internet, votre ami a besoin de votre adresse publique et du port UDP 28410 redirigé vers ce PC ; autorisez le jeu dans le pare-feu Windows.",
                           "Via Internet il tuo amico ha bisogno del tuo indirizzo pubblico e della porta UDP 28410 inoltrata a questo PC; consenti il gioco nel firewall di Windows.",
                           "Über das Internet braucht Ihr Freund Ihre öffentliche Adresse und den an diesen PC weitergeleiteten UDP-Port 28410; lassen Sie das Spiel in der Windows-Firewall zu.",
                           "Por Internet tu amigo necesita tu dirección pública y el puerto UDP 28410 redirigido a este PC; permite el juego en el Firewall de Windows.",
                           "インターネット経由では、相手にあなたのグローバルIPアドレスが必要で、UDPポート28410をこのPCへ転送する必要があります。Windowsファイアウォールでゲームを許可してください。",
                           "Через интернет другу нужен ваш внешний адрес и проброс UDP-порта 28410 на этот ПК; разрешите игру в брандмауэре Windows.",
                           "Przez internet znajomy potrzebuje twojego publicznego adresu i portu UDP 28410 przekierowanego na ten komputer; zezwól na grę w Zaporze systemu Windows.",
                           "Via internet heeft je vriend je publieke adres nodig en moet UDP-poort 28410 naar deze pc worden doorgestuurd; sta het spel toe in Windows Firewall.",
                           "Pela Internet o seu amigo precisa do seu endereço público e da porta UDP 28410 reencaminhada para este PC; permita o jogo na Firewall do Windows.",
                           "Pela internet seu amigo precisa do seu endereço público e da porta UDP 28410 redirecionada para este PC; permita o jogo no Firewall do Windows.",
                           "通过互联网联机时，好友需要你的公网地址，并需将 UDP 端口 28410 转发到这台电脑；请在 Windows 防火墙中允许本游戏。",
                           "透過網際網路連線時，好友需要你的公用位址，並需將 UDP 連接埠 28410 轉送到這台電腦；請在 Windows 防火牆中允許本遊戲。"},
    [L1T_NET_EP_ONLY] = {"Network co-op supports Episode 1 in this version, and your save continues outside it. Choose $3, or $2 with an Episode 1 save.",
                         "En réseau, le mode coopératif ne prend en charge que l'épisode 1 dans cette version, et votre sauvegarde continue ailleurs. Choisissez $3, ou $2 avec une sauvegarde de l'épisode 1.",
                         "In rete la cooperativa supporta solo l'Episodio 1 in questa versione e il tuo salvataggio prosegue altrove. Scegli $3, oppure $2 con un salvataggio dell'Episodio 1.",
                         "Netzwerk-Koop unterstützt in dieser Version nur Episode 1, und Ihr Spielstand geht woanders weiter. Wählen Sie $3 oder $2 mit einem Spielstand aus Episode 1.",
                         "En red, el cooperativo solo admite el Episodio 1 en esta versión y tu partida continúa fuera de él. Elige $3, o $2 con una partida del Episodio 1.",
                         "このバージョンのネットワーク協力プレイはエピソード1のみ対応ですが、あなたのセーブデータは別の場面から再開します。「$3」を選ぶか、エピソード1のセーブデータで「$2」を選んでください。",
                         "В этой версии сетевой кооператив работает только в эпизоде 1, а ваше сохранение уже дальше. Выберите «$3» или «$2» с сохранением эпизода 1.",
                         "Kooperacja sieciowa w tej wersji obsługuje tylko epizod 1, a twój zapis prowadzi gdzie indziej. Wybierz $3 albo $2 z zapisem z epizodu 1.",
                         "Netwerk-co-op ondersteunt in deze versie alleen aflevering 1, en je opslag gaat elders verder. Kies $3, of $2 met een opslag uit aflevering 1.",
                         "Nesta versão o cooperativo em rede só suporta o Episódio 1 e a sua gravação continua noutro ponto. Escolha $3, ou $2 com uma gravação do Episódio 1.",
                         "Nesta versão o cooperativo em rede só suporta o Episódio 1 e seu save continua em outro ponto. Escolha $3, ou $2 com um save do Episódio 1.",
                         "本版本的网络协力仅支持第1章，而你的存档在其他章节继续。请选择“$3”，或用第1章的存档选择“$2”。",
                         "本版本的網路協力僅支援第1章，而你的存檔在其他章節繼續。請選擇「$3」，或用第1章的存檔選擇「$2」。"},
    [L1T_NOTE_SAVE_NET] = {"A co-op game keeps its own save on each PC: your solo campaign save is not changed.",
                           "Une partie coop a sa propre sauvegarde sur chaque PC : votre sauvegarde de campagne solo n'est pas modifiée.",
                           "Una partita co-op ha un proprio salvataggio su ogni PC: il tuo salvataggio della campagna in solitario non cambia.",
                           "Ein Koop-Spiel hat auf jedem PC einen eigenen Spielstand: Ihr Solo-Kampagnen-Spielstand bleibt unverändert.",
                           "Una partida cooperativa tiene su propio guardado en cada PC: tu partida de la campaña en solitario no cambia.",
                           "協力プレイは各PCで専用のセーブデータを使います。ソロのキャンペーンのセーブデータは変わりません。",
                           "У кооперативной игры на каждом ПК своё сохранение: ваше сохранение одиночной кампании не меняется.",
                           "Gra w kooperacji ma na każdym komputerze własny zapis: twój zapis kampanii solo się nie zmienia.",
                           "Een co-opspel heeft op elke pc een eigen opslag: je solo-campagneopslag blijft ongewijzigd.",
                           "Um jogo cooperativo tem a sua própria gravação em cada PC: a sua gravação da campanha a solo não é alterada.",
                           "Um jogo cooperativo tem seu próprio save em cada PC: seu save da campanha solo não é alterado.",
                           "协力游戏在每台电脑上使用独立的存档：你的单人战役存档不会改变。", "協力遊戲在每台電腦上使用獨立的存檔：你的單人戰役存檔不會改變。"},
    [L1T_REF_PROTO] = {"The co-op mod versions do not match (network protocol). Both players need the same version.",
                       "Les versions du mod coop ne correspondent pas (protocole réseau). Il faut la même version des deux côtés.",
                       "Le versioni della mod co-op non corrispondono (protocollo di rete). Serve la stessa versione per entrambi.",
                       "Die Koop-Mod-Versionen passen nicht zusammen (Netzwerkprotokoll). Beide brauchen dieselbe Version.",
                       "Las versiones del mod cooperativo no coinciden (protocolo de red). Ambos necesitan la misma versión.",
                       "協力プレイ MOD のバージョンが違います (通信方式)。二人とも同じバージョンが必要です。", "Версии кооп-мода не совпадают (сетевой протокол). У обоих нужна одна версия.",
                       "Wersje moda co-op się różnią (protokół sieciowy). Obaj gracze potrzebują tej samej wersji.",
                       "De versies van de co-op-mod komen niet overeen (netwerkprotocol). Beide spelers hebben dezelfde versie nodig.",
                       "As versões do mod cooperativo não coincidem (protocolo de rede). Os dois precisam da mesma versão.",
                       "As versões do mod cooperativo não coincidem (protocolo de rede). Os dois precisam da mesma versão.", "协力模组版本不一致（网络协议）。双方需要相同的版本。",
                       "協力模組版本不一致（網路協定）。雙方需要相同的版本。"},
    [L1T_REF_MODVER] = {"Co-op mod versions differ: host %s, you %s. Start the game from the Co-op shortcut to update it.",
                        "Versions du mod coop différentes : hôte %s, vous %s. Lancez le jeu par le raccourci Co-op pour le mettre à jour.",
                        "Versioni della mod co-op diverse: host %s, tu %s. Avvia il gioco dal collegamento Co-op per aggiornarla.",
                        "Koop-Mod-Versionen verschieden: Host %s, Sie %s. Spiel über die Co-op-Verknüpfung starten, das aktualisiert sie.",
                        "Versiones del mod distintas: anfitrión %s, tú %s. Inicia el juego con el acceso directo Co-op para actualizarlo.",
                        "協力 MOD バージョン違い: ホスト %s、あなた %s。Co-op ショートカットで起動すると更新されます。", "Разные версии кооп-мода: хост %s, вы %s. Запуск ярлыком Co-op обновит мод.",
                        "Różne wersje moda co-op: host %s, ty %s. Uruchom grę skrótem Co-op, aby go zaktualizować.",
                        "Versies van de co-op-mod verschillen: host %s, jij %s. Start het spel via de Co-op-snelkoppeling om bij te werken.",
                        "Versões do mod diferentes: anfitrião %s, este PC %s. Inicie o jogo pelo atalho Co-op para o atualizar.",
                        "Versões do mod diferentes: anfitrião %s, você %s. Abra o jogo pelo atalho Co-op para atualizar.", "协力模组版本不同：主机 %s，你 %s。请通过 Co-op 快捷方式启动游戏以更新。",
                        "協力模組版本不同：主機 %s，你 %s。請透過 Co-op 捷徑啟動遊戲以更新。"},
    [L1T_REF_MODVER_OLD] = {"The host has an older co-op mod (you have %s). Start the game from the Co-op shortcut on both PCs to update it.",
                            "L'hôte a un mod coop plus ancien (vous : %s). Lancez le jeu par le raccourci Co-op sur les deux PC.",
                            "L'host ha una mod co-op più vecchia (tu: %s). Avviate il gioco dal collegamento Co-op su entrambi i PC.",
                            "Der Host hat eine ältere Koop-Mod (Sie: %s). Spiel auf beiden PCs über die Co-op-Verknüpfung starten.",
                            "El anfitrión tiene un mod más antiguo (tú: %s). Iniciad el juego con el acceso directo Co-op en ambos PC.",
                            "ホストの協力 MOD は古いバージョンです (あなた: %s)。両方の PC で Co-op から起動してください。", "У хоста старый кооп-мод (у вас %s). Запустите игру ярлыком Co-op на обоих ПК.",
                            "Host ma starszą wersję moda co-op (ty: %s). Uruchomcie grę skrótem Co-op na obu PC.",
                            "De host heeft een oudere co-op-mod (jij: %s). Start het spel op beide pc's via de Co-op-snelkoppeling.",
                            "O anfitrião tem um mod mais antigo (este PC: %s). Iniciem o jogo pelo atalho Co-op nos dois PCs.",
                            "O anfitrião tem um mod mais antigo (você: %s). Abram o jogo pelo atalho Co-op nos dois PCs.", "主机的协力模组版本较旧（你：%s）。请在两台电脑上通过 Co-op 快捷方式启动游戏。",
                            "主機的協力模組版本較舊（你：%s）。請在兩台電腦上透過 Co-op 捷徑啟動遊戲。"},
    [L1T_REF_BUILD] = {"Your partner runs another version of the game. Both need the current Steam version.",
                       "Votre partenaire utilise une autre version du jeu. Il faut la version Steam actuelle des deux côtés.",
                       "Il tuo compagno usa un'altra versione del gioco. Entrambi avete bisogno della versione Steam attuale.",
                       "Ihr Partner nutzt eine andere Version des Spiels. Beide brauchen die aktuelle Steam-Version.", "Tu compañero usa otra versión del juego. Ambos necesitáis la versión actual de Steam.",
                       "相手は別のバージョンのゲームを使っています。両方とも最新のSteam版が必要です。", "У напарника другая версия игры. Обоим нужна текущая версия из Steam.",
                       "Partner ma inną wersję gry. Obaj potrzebujecie aktualnej wersji ze Steam.", "Je partner gebruikt een andere versie van het spel. Beiden hebben de huidige Steam-versie nodig.",
                       "O seu parceiro usa outra versão do jogo. Ambos precisam da versão atual do Steam.", "Seu parceiro usa outra versão do jogo. Os dois precisam da versão atual da Steam.",
                       "对方的游戏版本不同。双方都需要当前的 Steam 版本。", "對方的遊戲版本不同。雙方都需要目前的 Steam 版本。"},
    [L1T_REF_STARTED] = {"This co-op game has already started. Joining a running game is not possible in this version.",
                         "Cette partie coopérative a déjà commencé. Rejoindre une partie en cours n'est pas possible dans cette version.",
                         "Questa partita cooperativa è già iniziata. In questa versione non è possibile unirsi a una partita in corso.",
                         "Dieses Koop-Spiel hat bereits begonnen. In dieser Version kann man einem laufenden Spiel nicht beitreten.",
                         "Esta partida cooperativa ya ha empezado. En esta versión no es posible unirse a una partida en curso.",
                         "この協力プレイはすでに始まっています。このバージョンでは、進行中のゲームには参加できません。", "Эта совместная игра уже началась. В этой версии нельзя подключиться к идущей игре.",
                         "Ta gra kooperacyjna już się rozpoczęła. W tej wersji nie można dołączyć do trwającej gry.", "Dit co-op-spel is al begonnen. Meedoen met een lopend spel kan in deze versie niet.",
                         "Este jogo cooperativo já começou. Nesta versão não é possível entrar num jogo em curso.",
                         "Este jogo cooperativo já começou. Nesta versão não é possível entrar em um jogo em andamento.", "此协力游戏已经开始。本版本不支持加入进行中的游戏。",
                         "此協力遊戲已經開始。本版本不支援加入進行中的遊戲。"},
    [L1T_REF_NOANSWER] = {"No answer from %s. Check the address, the host's firewall, and that the host pressed Start.",
                          "Aucune réponse de %s. Vérifiez l'adresse, le pare-feu de l'hôte, et que l'hôte a appuyé sur Démarrer.",
                          "Nessuna risposta da %s. Controlla l'indirizzo, il firewall dell'host e che l'host abbia premuto Avvia.",
                          "Keine Antwort von %s. Prüfen Sie die Adresse, die Firewall des Hosts und ob der Host Starten gedrückt hat.",
                          "Sin respuesta de %s. Revisa la dirección, el cortafuegos del anfitrión y que el anfitrión haya pulsado Iniciar.",
                          "%sから応答がありません。アドレス、ホストのファイアウォール、ホストが「開始」を押したかを確認してください。", "Нет ответа от %s. Проверьте адрес, брандмауэр хоста и что хост нажал «Начать».",
                          "Brak odpowiedzi od %s. Sprawdź adres, zaporę hosta i czy host nacisnął Uruchom.",
                          "Geen antwoord van %s. Controleer het adres, de firewall van de host en of de host op Starten heeft gedrukt.",
                          "Sem resposta de %s. Verifique o endereço, a firewall do anfitrião e se o anfitrião carregou em Iniciar.",
                          "Sem resposta de %s. Verifique o endereço, o firewall do anfitrião e se o anfitrião apertou Iniciar.", "%s 没有响应。请检查地址、主机的防火墙，以及主机是否已按“开始”。",
                          "%s 沒有回應。請檢查位址、主機的防火牆，以及主機是否已按「開始」。"},
    [L1T_WAIT_LOAD] = {"Waiting for %s to finish loading...", "En attente de %s : chargement en cours...", "In attesa che %s finisca il caricamento...", "Warte darauf, dass %s fertig geladen hat...",
                       "Esperando a que %s termine de cargar...", "%sの読み込み完了を待っています...", "Ожидание: %s ещё загружает уровень...", "Czekam, aż %s skończy wczytywanie...",
                       "Wachten tot %s klaar is met laden...", "A aguardar que %s termine de carregar...", "Aguardando %s terminar de carregar...", "正在等待%s完成加载...", "正在等待%s完成載入..."},
    [L1T_PARTNER_SILENT] = {"No answer from %s for %s s (game in the background, loading or stalled)...", "Aucune réponse de %s depuis %s s (jeu en arrière-plan, en chargement ou bloqué)...",
                            "Nessuna risposta da %s da %s s (gioco in background, in caricamento o bloccato)...", "Keine Antwort von %s seit %s s (Spiel im Hintergrund, lädt oder hängt)...",
                            "Sin respuesta de %s desde hace %s s (juego en segundo plano, cargando o bloqueado)...", "%sから%s秒間応答がありません (バックグラウンド、読み込み中、または停止中)...",
                            "Нет ответа от %s уже %s с (игра свернута, загружается или зависла)...", "Brak odpowiedzi od %s od %s s (gra w tle, wczytuje się lub zawiesiła się)...",
                            "Geen antwoord van %s sinds %s s (spel op de achtergrond, aan het laden of vastgelopen)...", "Sem resposta de %s há %s s (jogo em segundo plano, a carregar ou bloqueado)...",
                            "Sem resposta de %s há %s s (jogo em segundo plano, carregando ou travado)...", "%s已有%s秒无响应（游戏在后台、正在加载或卡住）...",
                            "%s已有%s秒無回應（遊戲在背景、正在載入或卡住）..."},
    [L1T_CONTINUES_ALONE] = {"Co-op has ended; your game goes on.", "La coopération est terminée ; votre partie continue.", "La cooperativa è terminata; la tua partita continua.",
                             "Koop ist beendet; Ihr Spiel geht weiter.", "El cooperativo ha terminado; tu partida continúa.", "協力プレイは終了しました。ゲームはそのまま続きます。",
                             "Совместная игра окончена; ваша игра продолжается.", "Kooperacja zakończona; twoja gra trwa dalej.", "Co-op is afgelopen; je spel gaat door.",
                             "O modo cooperativo terminou; o seu jogo continua.", "O modo cooperativo terminou; seu jogo continua.", "协力已结束，你的游戏继续进行。", "協力已結束，你的遊戲繼續進行。"},
    [L1T_HOST_ENDED] = {"The host has ended the session.", "L'hôte a mis fin à la session.", "L'host ha terminato la sessione.", "Der Host hat die Sitzung beendet.", "El anfitrión ha terminado la sesión.",
                        "ホストがセッションを終了しました。", "Хост завершил сессию.", "Host zakończył sesję.", "De host heeft de sessie beëindigd.", "O anfitrião terminou a sessão.",
                        "O anfitrião encerrou a sessão.", "主机已结束本次联机。", "主機已結束本次連線。"},
    [L1T_TO_TITLE] = {"Returning to the title screen...", "Retour à l'écran titre...", "Ritorno alla schermata del titolo...", "Zurück zum Titelbildschirm...", "Volviendo a la pantalla de título...",
                      "タイトル画面に戻ります…", "Возврат на титульный экран...", "Powrót do ekranu tytułowego...", "Terug naar het titelscherm...", "A regressar ao ecrã de título...",
                      "Voltando à tela de título...", "正在返回标题画面……", "正在返回標題畫面……"},
    [L1T_KICK] = {"Remove partner", "Expulser le partenaire", "Espelli il compagno", "Partner entfernen", "Expulsar al compañero", "パートナーを追放", "Исключить напарника", "Usuń partnera",
                  "Partner verwijderen", "Expulsar parceiro", "Expulsar parceiro", "驱逐搭档", "驅逐搭檔"},
    [L1T_EP_ENDED] = {"Network co-op supports Episode 1 only in this version: the session has ended, your game goes on alone.",
                      "En réseau, le mode coopératif ne prend en charge que l'épisode 1 dans cette version : la session est terminée, votre partie continue seule.",
                      "In rete la cooperativa supporta solo l'Episodio 1 in questa versione: la sessione è terminata, la tua partita continua da sola.",
                      "Netzwerk-Koop unterstützt in dieser Version nur Episode 1: Die Sitzung ist beendet, Ihr Spiel geht allein weiter.",
                      "En red, el cooperativo solo admite el Episodio 1 en esta versión: la sesión ha terminado y tu partida sigue en solitario.",
                      "このバージョンのネットワーク協力プレイはエピソード1のみ対応です。セッションは終了し、ゲームは1人で続きます。",
                      "Сетевой кооператив в этой версии поддерживает только эпизод 1: сессия завершена, игра продолжается в одиночку.",
                      "Kooperacja sieciowa w tej wersji obsługuje tylko epizod 1: sesja zakończona, gra toczy się dalej w pojedynkę.",
                      "Netwerk-co-op ondersteunt in deze versie alleen aflevering 1: de sessie is beëindigd, je spel gaat alleen verder.",
                      "Nesta versão o cooperativo em rede só suporta o Episódio 1: a sessão terminou e o seu jogo continua a solo.",
                      "Nesta versão o cooperativo em rede só suporta o Episódio 1: a sessão foi encerrada e seu jogo continua sozinho.",
                      "本版本的网络协力仅支持第1章：本次联机已结束，你的游戏将单人继续。", "本版本的網路協力僅支援第1章：本次連線已結束，你的遊戲將單人繼續。"},
    /* round 2: the name a PC sends when Steam gives no persona name (the partner sees it in his lobby and strips), in the sender's language */
    [L1T_NAME_P1] = {"Player 1", "Joueur 1", "Giocatore 1", "Spieler 1", "Jugador 1", "プレイヤー1", "Игрок 1", "Gracz 1", "Speler 1", "Jogador 1", "Jogador 1", "玩家1", "玩家1"},
    [L1T_NAME_P2] = {"Player 2", "Joueur 2", "Giocatore 2", "Spieler 2", "Jugador 2", "プレイヤー2", "Игрок 2", "Gracz 2", "Speler 2", "Jogador 2", "Jogador 2", "玩家2", "玩家2"},
    /* round 2: the Steam way */
    [L1T_NET_WAY] = {"Connection", "Connexion", "Connessione", "Verbindung", "Conexión", "接続方法", "Подключение", "Połączenie", "Verbinding", "Ligação", "Conexão", "连接方式", "連線方式"},
    [L1T_WAY_IP] = {"Direct IP", "IP directe", "IP diretto", "Direkte IP", "IP directa", "IPアドレスで直接", "Прямой IP", "Bezpośrednie IP", "Rechtstreeks via IP", "IP direto", "IP direto", "直接IP", "直接IP"},
    [L1T_WAY_STEAM] = {"Steam friends", "Amis Steam", "Amici di Steam", "Steam-Freunde", "Amigos de Steam", "Steamフレンド", "Друзья в Steam", "Znajomi ze Steam", "Steam-vrienden", "Amigos Steam", "Amigos da Steam", "Steam好友", "Steam好友"},
    [L1T_STEAM_HINT] = {"Friends join from their Steam friends list (Join Game); no address, no port forwarding.",
                        "Vos amis rejoignent depuis leur liste d'amis Steam (Rejoindre la partie) ; ni adresse ni redirection de port.",
                        "Gli amici si uniscono dall'elenco amici di Steam (Partecipa); nessun indirizzo, nessun port forwarding.",
                        "Freunde treten über ihre Steam-Freundesliste bei (Spiel beitreten); keine Adresse, keine Portweiterleitung.",
                        "Tus amigos se unen desde su lista de amigos de Steam (Unirse a la partida); sin dirección ni reenvío de puertos.",
                        "フレンドはSteamのフレンドリスト（ゲームに参加）から参加します。アドレスもポート開放も不要です。",
                        "Друзья подключаются из списка друзей Steam («Присоединиться к игре»); без адреса и проброса портов.",
                        "Znajomi dołączają z listy znajomych Steam (Dołącz do gry); bez adresu i przekierowania portów.",
                        "Vrienden doen mee via hun Steam-vriendenlijst (Deelnemen aan spel); geen adres, geen port forwarding.",
                        "Os amigos juntam-se pela lista de amigos da Steam (Entrar no jogo); sem endereço nem reencaminhamento de portas.",
                        "Os amigos entram pela lista de amigos da Steam (Entrar no jogo); sem endereço nem redirecionamento de portas.",
                        "好友从Steam好友列表（加入游戏）加入；无需地址，无需端口转发。",
                        "好友從Steam好友清單（加入遊戲）加入；無需位址，無需連接埠轉送。"},
    [L1T_NET_INVITE] = {"Invite a friend", "Inviter un ami", "Invita un amico", "Freund einladen", "Invitar a un amigo", "フレンドを招待", "Пригласить друга", "Zaproś znajomego", "Vriend uitnodigen", "Convidar um amigo", "Convidar um amigo", "邀请好友", "邀請好友"},
    [L1T_NET_STEAMID] = {"Host's Steam ID", "ID Steam de l'hôte", "ID Steam dell'host", "Steam-ID des Hosts", "ID de Steam del anfitrión", "ホストのSteam ID", "Steam ID хоста", "Steam ID hosta", "Steam-ID van de host", "ID Steam do anfitrião", "ID Steam do anfitrião", "主机的Steam ID", "主機的Steam ID"},
    [L1T_NET_STEAMID_HINT] = {"type it, or use Join Game in Steam", "saisissez-le, ou Rejoindre la partie dans Steam", "digitalo, o usa Partecipa in Steam",
                              "eintippen oder in Steam „Spiel beitreten“", "escríbelo o usa Unirse a la partida en Steam", "入力するか、Steamで「ゲームに参加」",
                              "введите его или «Присоединиться к игре» в Steam", "wpisz je lub użyj Dołącz do gry w Steam", "typ het in, of gebruik Deelnemen aan spel in Steam",
                              "escreva-o, ou use Entrar no jogo na Steam", "digite-o, ou use Entrar no jogo na Steam", "输入，或在Steam中点“加入游戏”", "輸入，或在Steam中點「加入遊戲」"},
    /* ep1-flow: the Downed / revive row (E12; the label is the Revelations 2 wording where that mod has the language) and the end of Episode 1 (E5). Our wording, not
     * read by native speakers. "Off" of the row and "Episode 1" of the strip are the game's own words first (L1K_OFF, L1K_EPISODE1). */
    [L1T_DOWNED] = {"Downed / revive", "Agonie / réanimation", "Agonia / rianimazione", "Agonie / Wiederbelebung", "Agonía / reanimación", "瀕死・蘇生", "Агония / поднять напарника",
                    "Agonia / podnoszenie", "Neergaan / reanimeren", "Agonia / reanimar", "Agonia / reviver", "濒死 / 救助", "瀕死 / 救助"},
    [L1T_SECS] = {"%s s", "%s s", "%s s", "%s s", "%s s", "%s秒", "%s с", "%s s", "%s s", "%s s", "%s s", "%s秒", "%s秒"},
    [L1T_NO_LIMIT] = {"No time limit", "Sans limite de temps", "Senza limite di tempo", "Ohne Zeitlimit", "Sin límite de tiempo", "時間制限なし", "Без ограничения времени", "Bez limitu czasu",
                      "Geen tijdslimiet", "Sem limite de tempo", "Sem limite de tempo", "无时间限制", "無時間限制"},
    [L1T_EPISODE1] = {"Episode 1", "Épisode 1", "Episodio 1", "Episode 1", "Episodio 1", "Episode 1", "Эпизод 1", "Epizod 1", "Episode 1", "Episódio 1", "Episódio 1", "第1章", "第1章"},
    [L1T_EP1_DONE] = {"%s complete! The co-op session has ended; your game goes on alone.", "%s terminé ! La session coopérative est terminée ; votre partie continue en solo.",
                      "%s completato! La sessione cooperativa è terminata; la tua partita continua da solo.", "%s abgeschlossen! Die Koop-Sitzung ist beendet; Ihr Spiel geht allein weiter.",
                      "¡%s completado! La sesión cooperativa ha terminado; tu partida continúa en solitario.", "%sクリア！協力プレイのセッションは終了しました。ここからは一人でプレイします。",
                      "%s пройден! Совместная сессия завершена, дальше вы играете один.", "%s ukończony! Sesja kooperacji zakończyła się; gra toczy się dalej w pojedynkę.",
                      "%s voltooid! De co-opsessie is beëindigd; je spel gaat alleen verder.", "%s concluído! A sessão cooperativa terminou; o seu jogo continua a solo.",
                      "%s concluído! A sessão cooperativa terminou; seu jogo continua sozinho.", "%s完成！合作会话已结束，你的游戏将单人继续。", "%s完成！合作會話已結束，你的遊戲將單人繼續。"},
    [L1T_EP1_BEHIND] = {"%s: the host has reached the end. The co-op session has ended.", "%s : l'hôte est arrivé à la fin. La session coopérative est terminée.",
                        "%s: l'host è arrivato alla fine. La sessione cooperativa è terminata.", "%s: Der Host hat das Ende erreicht. Die Koop-Sitzung ist beendet.",
                        "%s: el anfitrión ha llegado al final. La sesión cooperativa ha terminado.", "%s：ホストが最後まで到達しました。協力プレイのセッションは終了しました。",
                        "%s: хост дошёл до конца. Совместная сессия завершена.", "%s: host dotarł do końca. Sesja kooperacji zakończyła się.",
                        "%s: de host heeft het einde bereikt. De co-opsessie is beëindigd.", "%s: o anfitrião chegou ao fim. A sessão cooperativa terminou.",
                        "%s: o anfitrião chegou ao fim. A sessão cooperativa terminou.", "%s：主机已到达终点。合作会话已结束。", "%s：主機已到達終點。合作會話已結束。"},
    /* ep1-solo E3: the 1-3 solo part (the story switches the partner off until the reunion): the other player watches */
    [L1T_SOLO_WATCH] = {"For now %s goes on without you: you are watching until you meet again.", "Pour l'instant, %s continue sans vous : vous regardez jusqu'à vos retrouvailles.",
                        "Per ora %s prosegue senza di te: stai guardando finché non vi ritroverete.", "Vorerst geht es für %s ohne dich weiter: Du siehst zu, bis ihr euch wiederseht.",
                        "Por ahora, %s sigue sin ti: estás mirando hasta que os volváis a encontrar.", "しばらくは%sが単独で進みます。合流するまで観戦しています。",
                        "Пока %s продолжает без вас: вы наблюдаете до встречи.", "Na razie %s idzie dalej bez ciebie: oglądasz do waszego ponownego spotkania.",
                        "Voorlopig gaat %s zonder jou verder: je kijkt mee tot jullie elkaar weer treffen.", "Por agora, %s continua sem si: está a assistir até se voltarem a encontrar.",
                        "Por enquanto, %s segue sem você: você está assistindo até vocês se reencontrarem.", "%s暂时独自行动：在你们会合之前，你在一旁观看。", "%s暫時獨自行動：在你們會合之前，你在一旁觀看。"},
    /* rel010: the friend row of Join + Steam friends (our wording, not read by native speakers) */
    [L1T_NET_FRIEND] = {"Friend hosting", "Ami qui héberge", "Amico che ospita", "Freund als Host", "Amigo anfitrión", "ホスト中のフレンド", "Друг-хост", "Znajomy hostuje",
                        "Vriend als host", "Amigo anfitrião", "Amigo hospedando", "主持中的好友", "主持中的好友"},
    [L1T_NET_NOFRIEND] = {"no friend is hosting now", "aucun ami n'héberge en ce moment", "nessun amico sta ospitando", "gerade hostet kein Freund", "ningún amigo aloja ahora",
                          "ホスト中のフレンドはいません", "сейчас никто из друзей не хостит", "teraz żaden znajomy nie hostuje", "geen vriend host nu", "nenhum amigo aloja agora",
                          "nenhum amigo hospedando agora", "目前没有好友在主持", "目前沒有好友在主持"},
    /* rev1 live 16: Continue keeps the roles of the co-op save ($1 Continue and $3 New Game are the game's own words, the names the game's own; our wording) */
    [L1T_CONT_ROLES] = {"$1 keeps the characters of the co-op save. Your character: %s.", "$1 garde les personnages de la sauvegarde coop. Votre personnage : %s.",
                        "$1 mantiene i personaggi del salvataggio co-op. Il tuo personaggio: %s.", "$1 behält die Figuren des Koop-Spielstands. Ihre Figur: %s.",
                        "$1 mantiene los personajes de la partida cooperativa. Tu personaje: %s.", "「$1」は協力プレイのセーブデータのキャラクターで続けます。あなたのキャラクター：%s。",
                        "«$1» продолжает с персонажами кооперативного сохранения. Ваш персонаж: %s.", "$1 zachowuje postacie z zapisu kooperacji. Twoja postać: %s.",
                        "$1 houdt de personages van de co-opopslag. Jouw personage: %s.", "$1 mantém as personagens da gravação cooperativa. A sua personagem: %s.",
                        "$1 mantém os personagens do save cooperativo. Seu personagem: %s.", "“$1”沿用协力存档中的角色。你的角色：%s。", "「$1」沿用協力存檔中的角色。你的角色：%s。"},
    [L1T_NOTE_CONT_ROLES] = {"$1 of the co-op save keeps its characters: you play %s, your partner %s. $3 keeps the choice made here.",
                             "$1 de la sauvegarde coop garde ses personnages : vous jouez %s, votre partenaire %s. $3 garde le choix fait ici.",
                             "$1 del salvataggio co-op mantiene i suoi personaggi: tu sei %s, il partner %s. $3 mantiene la scelta fatta qui.",
                             "$1 des Koop-Spielstands behält seine Figuren: Sie spielen %s, Ihr Partner %s. $3 behält die Wahl von hier.",
                             "$1 de la partida cooperativa mantiene sus personajes: tú eres %s y tu compañero %s. $3 mantiene lo elegido aquí.",
                             "協力プレイのセーブデータの「$1」はそのキャラクターで続けます（あなた：%s、パートナー：%s）。「$3」ならここでの選択のままです。",
                             "«$1» кооперативного сохранения идёт с его персонажами: ваш — %s, напарника — %s. «$3» — с выбором на этом экране.",
                             "$1 zapisu kooperacji zachowuje jego postacie: ty grasz jako %s, partner jako %s. $3 zachowuje wybór z tego ekranu.",
                             "$1 van de co-opopslag houdt zijn personages: jij speelt %s, je partner %s. $3 houdt de keuze van hier.",
                             "$1 da gravação cooperativa mantém as suas personagens: joga com %s, o parceiro com %s. $3 mantém a escolha feita aqui.",
                             "$1 do save cooperativo mantém os personagens dele: você joga com %s, o parceiro com %s. $3 mantém a escolha feita aqui.",
                             "协力存档的“$1”沿用其中的角色：你是%s，搭档是%s。“$3”则沿用这里的选择。", "協力存檔的「$1」沿用其中的角色：你是%s，搭檔是%s。「$3」則沿用這裡的選擇。"},
};

/* ---- the game's own texts (Coop_<lang>.gmd) ---- */
const char *const l1_stock_labels[L1K_COUNT] = {"coop_error_mes_01", "coop_error_mes_09", "coop_error_mes_12", "coop_guide_mes_10", "coop_guide_mes_27",
                                                "coop_join", "coop_ready", "coop_notice_kick", "partner_waiting_info", "partner_ready", "partner_recruit",
                                                "game_setting_menu_01_02", "episode_01"};
const char *const l1_stock_banks[L1K_COUNT] = {"Coop", "Coop", "Coop", "Coop", "Coop", "Coop", "Coop", "Coop", "Coop", "Coop", "Coop", "option", "campaign"};
static const char *const STOCK_EN[L1K_COUNT] = {"The session you are trying to join is already full.", "Connection to the other player has been terminated.", "Failed to join session.",
                                                "%s has left.", "You have been removed from the Lobby by the host.", "%s has joined.", "%s is ready.", "Do you want to kick %s?",
                                                "Waiting for other player.", "Ready", "Waiting", "Off", "Episode 1"};
static char g_stock[L1K_COUNT][256];

/* the end of a UTF-8 string cut at most at `max` bytes, never inside a character */
static size_t utf8_cut(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

/* drop an incomplete UTF-8 sequence at the end (what a byte limit can leave) */
static void utf8_trim_tail(char *s)
{
    size_t n = strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;   /* back to the lead byte of the last character */
    if (i == 0) {
        if (n) s[0] = 0;
        return;
    }
    unsigned char lead = (unsigned char)s[i - 1];
    size_t want = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
    if (want == 0 || n - (i - 1) < want) s[i - 1] = 0;
}

void l1_stock_clean(const char *gmd, int lang, char *out, size_t size)
{
    size_t o = 0;
    int cjk = lang == 5 || lang == 11 || lang == 12, user = 0;
    if (!size) return;
    out[0] = 0;
    if (!gmd) return;
#define PUT(c) do { if (o + 1 < size) out[o++] = (char)(c); } while (0)
    for (const char *p = gmd; *p;) {
        unsigned char c = (unsigned char)*p;
        if (c == '<') {   /* a tag: <USER> is the name, everything else (<SIZE 18>, <COL ...>, </COL>, ...) goes */
            const char *e = strchr(p, '>');
            if (!e) break;
            if (e - p == 5 && !strncmp(p, "<USER", 5) && !user && o + 3 < size) out[o++] = '%', out[o++] = 's', user = 1;
            p = e + 1;
            continue;
        }
        int nl = c == '\n' || c == '\r' || (c == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0xA8);   /* \r \n and U+2028 */
        if (nl) {
            p += c == 0xE2 ? 3 : 1;
            while (*p == '\r' || *p == '\n' || ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x80 && (unsigned char)p[2] == 0xA8)) p += *p == '\r' || *p == '\n' ? 1 : 3;
            if (o && out[o - 1] == '-' && *p >= 'a' && *p <= 'z' && o >= 2 && out[o - 2] != ' ') o--;   /* "deel-\nnemen": a word split by the line break */
            else if (!cjk && o && out[o - 1] != ' ') PUT(' ');
            continue;
        }
        if (c == '%') {
            if (o + 2 < size) out[o++] = '%', out[o++] = '%';
            p++;
            continue;
        }
        if (c == ' ' && o && out[o - 1] == ' ') { p++; continue; }
        PUT(c);
        p++;
    }
#undef PUT
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    utf8_trim_tail(out);   /* a cut at the end of the buffer must not leave half a character */
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

void l1_set_stock_text(int id, const char *clean)
{
    if (id < 0 || id >= L1K_COUNT) return;
    g_stock[id][0] = 0;
    if (!clean || !clean[0]) return;
    /* at most one %s and no other conversion: anything else keeps the built-in English */
    int n = 0, bad = 0;
    for (const char *p = clean; *p; p++)
        if (*p == '%') {
            if (p[1] == 's') n++, p++;
            else if (p[1] == '%') p++;
            else bad = 1;
        }
    if (bad || n > 1) return;
    size_t k = utf8_cut(clean, sizeof g_stock[id] - 1);
    memcpy(g_stock[id], clean, k);
    g_stock[id][k] = 0;
}

const char *l1_stock(int id)
{
    if (id < 0 || id >= L1K_COUNT) return "";
    return g_stock[id][0] ? g_stock[id] : STOCK_EN[id];
}

int l1_stock_have(int id) { return id >= 0 && id < L1K_COUNT && g_stock[id][0] != 0; }

void l1_fmt(char *out, size_t size, const char *tmpl, const char *a1, const char *a2)
{
    size_t o = 0;
    int used = 0;
    if (!size) return;
    out[0] = 0;
    if (!tmpl) return;
    for (const char *p = tmpl; *p && o + 1 < size;) {
        if (p[0] == '%' && p[1] == '%') {
            out[o++] = '%', p += 2;
            continue;
        }
        if (p[0] == '%' && p[1] == 's') {
            const char *a = used == 0 ? a1 : used == 1 ? a2 : NULL;
            used++;
            for (; a && *a && o + 1 < size; a++) out[o++] = *a;
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    utf8_trim_tail(out);
}

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
