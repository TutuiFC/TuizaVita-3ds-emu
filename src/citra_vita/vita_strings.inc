// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Textos de los menus (0.3.3.9). Una entrada por texto, con los idiomas en el
// orden de VitaFrontend::I18n::Language:
//   TR(id, ingles, espanol, catalan, frances, aleman, italiano, portugues)
// Los "%d", "%u" y "%s" tienen que salir en el MISMO orden en todos los
// idiomas: se pasan a snprintf. Sin "oe" ligado ni caracteres fuera de
// Latin-1: la fuente latina de la consola no los trae.

// --- Menu de inicio ----------------------------------------------------------
TR(kHomeNoGames,
   "No games in ux0:/data/azahar/roms",
   "No hay juegos en ux0:/data/azahar/roms",
   "No hi ha jocs a ux0:/data/azahar/roms",
   "Aucun jeu dans ux0:/data/azahar/roms",
   "Keine Spiele in ux0:/data/azahar/roms",
   "Nessun gioco in ux0:/data/azahar/roms",
   "Nenhum jogo em ux0:/data/azahar/roms")
TR(kHomeNoGamesHint,
   "Copy .3ds / .cci / .cxi / .app / .3dsx files there and restart the app.",
   "Copia ahí ficheros .3ds / .cci / .cxi / .app / .3dsx y vuelve a abrir la app.",
   "Copia-hi fitxers .3ds / .cci / .cxi / .app / .3dsx i torna a obrir l'app.",
   "Copiez-y des fichiers .3ds / .cci / .cxi / .app / .3dsx et relancez l'app.",
   "Kopiere .3ds / .cci / .cxi / .app / .3dsx dorthin und starte die App neu.",
   "Copia lì file .3ds / .cci / .cxi / .app / .3dsx e riapri l'app.",
   "Copie arquivos .3ds / .cci / .cxi / .app / .3dsx para lá e reabra o app.")
TR(kHomeNoInfo,
   "No title data in this file (encrypted ROM or homebrew without icon)",
   "Este fichero no trae datos del título (ROM cifrada o homebrew sin icono)",
   "Aquest fitxer no porta dades del títol (ROM xifrada o homebrew sense icona)",
   "Pas de données de titre (ROM chiffrée ou homebrew sans icône)",
   "Keine Titeldaten (verschlüsselte ROM oder Homebrew ohne Symbol)",
   "Nessun dato del titolo (ROM cifrata o homebrew senza icona)",
   "Sem dados do título (ROM criptografada ou homebrew sem ícone)")
TR(kHomeNoShot,
   "The screen will show up here after you play",
   "La pantalla saldrá aquí cuando juegues",
   "La pantalla sortirà aquí quan juguis",
   "L'écran s'affichera ici après avoir joué",
   "Der Bildschirm erscheint hier nach dem Spielen",
   "Lo schermo apparirà qui dopo aver giocato",
   "A tela aparecerá aqui depois que você jogar")
TR(kHomePlay, "Play", "Jugar", "Jugar", "Jouer", "Spielen", "Gioca", "Jogar")
TR(kHomeExit, "Exit", "Salir", "Sortir", "Quitter", "Beenden", "Esci", "Sair")
TR(kHomeSettingsHold,
   "Settings (hold)", "Ajustes (mantén)", "Ajustos (mantén)", "Paramètres (maintenir)",
   "Einstellungen (halten)", "Impostazioni (tieni)", "Configurações (segure)")
TR(kHomeLanguageHold,
   "Language (hold)", "Idioma (mantén)", "Idioma (mantén)", "Langue (maintenir)",
   "Sprache (halten)", "Lingua (tieni)", "Idioma (segure)")

// --- Selector de idioma ------------------------------------------------------
TR(kLanguageTitle, "Language", "Idioma", "Idioma", "Langue", "Sprache", "Lingua", "Idioma")
TR(kLanguageSubtitle,
   "Choose the language of the menus.",
   "Elige el idioma de los menús.",
   "Tria l'idioma dels menús.",
   "Choisissez la langue des menus.",
   "Wähle die Sprache der Menüs.",
   "Scegli la lingua dei menu.",
   "Escolha o idioma dos menus.")
TR(kConfirm, "Confirm", "Aceptar", "Accepta", "Valider", "Bestätigen", "Conferma", "Confirmar")
TR(kBack, "Back", "Volver", "Torna", "Retour", "Zurück", "Indietro", "Voltar")

// --- Ajustes -----------------------------------------------------------------
TR(kSettingsTitle, "Settings", "Ajustes", "Ajustos", "Paramètres", "Einstellungen",
   "Impostazioni", "Configurações")
TR(kSettingsSubtitle,
   "Saved in ux0:/data/azahar/ajustes.txt. FPS figures are approximate and depend on the game.",
   "Se guardan en ux0:/data/azahar/ajustes.txt. Las cifras de FPS son aproximadas y dependen del juego.",
   "Es desen a ux0:/data/azahar/ajustes.txt. Les xifres de FPS són aproximades i depenen del joc.",
   "Enregistrés dans ux0:/data/azahar/ajustes.txt. Les FPS sont approximatifs et dépendent du jeu.",
   "Gespeichert in ux0:/data/azahar/ajustes.txt. FPS-Angaben sind ungefähr und spielabhängig.",
   "Salvate in ux0:/data/azahar/ajustes.txt. Gli FPS sono indicativi e dipendono dal gioco.",
   "Salvas em ux0:/data/azahar/ajustes.txt. Os FPS são aproximados e dependem do jogo.")
TR(kSettingsFooter,
   "UP/DOWN: choose   LEFT/RIGHT: change   CIRCLE or START: save and return",
   "ARRIBA/ABAJO: elegir   IZQ/DER: cambiar   CÍRCULO o START: guardar y volver",
   "AMUNT/AVALL: triar   ESQ/DRETA: canviar   CERCLE o START: desar i tornar",
   "HAUT/BAS : choisir   GAUCHE/DROITE : modifier   ROND ou START : enregistrer",
   "OBEN/UNTEN: wählen   LINKS/RECHTS: ändern   KREIS oder START: speichern",
   "SU/GIÙ: scegli   SINISTRA/DESTRA: cambia   CERCHIO o START: salva ed esci",
   "CIMA/BAIXO: escolher   ESQ/DIR: mudar   CÍRCULO ou START: salvar e voltar")

// Las filas, en el orden del menu (ver kSettingsRows en main.cpp).
TR(kSetLabel0, "Volume", "Volumen", "Volum", "Volume", "Lautstärke", "Volume", "Volume")
TR(kSetLabel1, "Console language (games)", "Idioma de la consola (juegos)",
   "Idioma de la consola (jocs)", "Langue de la console (jeux)", "Konsolensprache (Spiele)",
   "Lingua della console (giochi)", "Idioma do console (jogos)")
TR(kSetLabel2, "Resolution (software)", "Resolución (software)", "Resolució (programari)",
   "Résolution (logiciel)", "Auflösung (Software)", "Risoluzione (software)",
   "Resolução (software)")
TR(kSetLabel3, "JIT register cache", "Caché de registros JIT", "Memòria cau de registres JIT",
   "Cache de registres JIT", "JIT-Registercache", "Cache registri JIT", "Cache de registradores JIT")
TR(kSetLabel4, "Floating point in JIT", "Coma flotante en JIT", "Coma flotant al JIT",
   "Virgule flottante (JIT)", "Gleitkomma im JIT", "Virgola mobile nel JIT",
   "Ponto flutuante no JIT")
TR(kSetLabel5, "No wait for the GPU", "Sin espera a la GPU", "Sense espera a la GPU",
   "Sans attente du GPU", "Kein Warten auf die GPU", "Senza attesa della GPU",
   "Sem espera pela GPU")
TR(kSetLabel6, "Direct presentation", "Presentar directo", "Presentació directa",
   "Présentation directe", "Direkte Ausgabe", "Presentazione diretta", "Apresentação direta")
TR(kSetLabel7, "JIT direct linking", "Enlace directo JIT", "Enllaç directe JIT",
   "Liaison directe JIT", "Direkte JIT-Verknüpfung", "Collegamento diretto JIT",
   "Ligação direta JIT")
TR(kSetLabel8, "Branching vertices on GPU", "Vértices con saltos a GPU",
   "Vèrtexs amb salts a la GPU", "Sommets avec sauts (GPU)", "Vertices mit Sprüngen (GPU)",
   "Vertici con salti su GPU", "Vértices com saltos na GPU")
TR(kSetLabel9, "Full vertex cache", "Caché de vértices completa", "Memòria cau de vèrtexs",
   "Cache de sommets complet", "Vollständiger Vertex-Cache", "Cache vertici completa",
   "Cache de vértices completo")
TR(kSetLabel10, "Sound", "Sonido", "So", "Son", "Ton", "Audio", "Som")
TR(kSetLabel11, "Screens", "Pantallas", "Pantalles", "Écrans", "Bildschirme", "Schermi", "Telas")
TR(kSetLabel12, "Skip repeated frames", "Omitir imágenes repetidas", "Ometre imatges repetides",
   "Ignorer images répétées", "Gleiche Bilder überspringen", "Salta immagini ripetute",
   "Pular imagens repetidas")
TR(kSetLabel13, "Specialized vertices on GPU", "Vértices especializados a GPU",
   "Vèrtexs especialitzats a la GPU", "Sommets spécialisés (GPU)", "Spezialisierte Vertices (GPU)",
   "Vertici specializzati su GPU", "Vértices especializados na GPU")
TR(kSetLabel14, "GPU copy and clear", "Copia y borrado en GPU", "Còpia i esborrat a la GPU",
   "Copie et effacement (GPU)", "Kopieren und Löschen (GPU)", "Copia e pulizia su GPU",
   "Cópia e limpeza na GPU")
TR(kSetLabel15, "GPU on another core", "GPU en otro núcleo", "GPU en un altre nucli",
   "GPU sur un autre coeur", "GPU auf anderem Kern", "GPU su un altro core",
   "GPU em outro núcleo")
TR(kSetLabel16, "Speed limit", "Límite de velocidad", "Límit de velocitat", "Limite de vitesse",
   "Tempolimit", "Limite di velocità", "Limite de velocidade")
TR(kSetLabel17, "GPU resolution", "Resolución GPU", "Resolució GPU", "Résolution GPU",
   "GPU-Auflösung", "Risoluzione GPU", "Resolução da GPU")
TR(kSetLabel18, "Asynchronous shaders", "Shaders asíncronos", "Shaders asíncrons",
   "Shaders asynchrones", "Asynchrone Shader", "Shader asincroni", "Shaders assíncronos")
TR(kSetLabel19, "Graphics engine", "Motor gráfico", "Motor gràfic", "Moteur graphique",
   "Grafik-Engine", "Motore grafico", "Motor gráfico")

// Tres lineas de ayuda por fila: que hace, que se gana, que se pierde.
TR(kSetHelp0a,
   "Game volume. Applied immediately.",
   "Volumen del juego. Se aplica al momento.",
   "Volum del joc. S'aplica a l'instant.",
   "Volume du jeu. Appliqué immédiatement.",
   "Lautstärke des Spiels. Gilt sofort.",
   "Volume del gioco. Si applica subito.",
   "Volume do jogo. Aplicado na hora.")
TR(kSetHelp0b, "Does not change FPS.", "No cambia los FPS.", "No canvia els FPS.",
   "Ne change pas les FPS.", "Ändert die FPS nicht.", "Non cambia gli FPS.", "Não muda o FPS.")
TR(kSetHelp0c, "", "", "", "", "", "", "")
TR(kSetHelp1a,
   "The language the game sees. Applied when the game starts.",
   "El idioma que ve el juego. Se aplica al arrancar el juego.",
   "L'idioma que veu el joc. S'aplica en arrencar el joc.",
   "La langue que voit le jeu. Appliqué au lancement du jeu.",
   "Die Sprache, die das Spiel sieht. Gilt beim Spielstart.",
   "La lingua vista dal gioco. Si applica all'avvio del gioco.",
   "O idioma que o jogo vê. Aplicado ao iniciar o jogo.")
TR(kSetHelp1b, "Does not change FPS.", "No cambia los FPS.", "No canvia els FPS.",
   "Ne change pas les FPS.", "Ändert die FPS nicht.", "Non cambia gli FPS.", "Não muda o FPS.")
TR(kSetHelp1c,
   "If the game doesn't include that language, it uses the first one of its region.",
   "Si el juego no trae ese idioma, usa el primero de su región.",
   "Si el joc no porta aquest idioma, fa servir el primer de la seva regió.",
   "Si le jeu n'a pas cette langue, il prend la première de sa région.",
   "Hat das Spiel diese Sprache nicht, nimmt es die erste seiner Region.",
   "Se il gioco non ha quella lingua, usa la prima della sua regione.",
   "Se o jogo não tiver esse idioma, usa o primeiro da sua região.")
TR(kSetHelp2a,
   "0.5x: the CPU draws 1 of every 4 pixels and copies the rest.",
   "0.5x: la CPU dibuja 1 de cada 4 píxeles y copia el resto.",
   "0.5x: la CPU dibuixa 1 de cada 4 píxels i copia la resta.",
   "0.5x : le CPU dessine 1 pixel sur 4 et copie le reste.",
   "0.5x: die CPU zeichnet 1 von 4 Pixeln und kopiert den Rest.",
   "0.5x: la CPU disegna 1 pixel su 4 e copia il resto.",
   "0.5x: a CPU desenha 1 de cada 4 pixels e copia o resto.")
TR(kSetHelp2b,
   "More FPS ONLY in games drawn in software (not regular 3D).",
   "Más FPS SOLO en juegos que dibujan por software (no los 3D normales).",
   "Més FPS NOMÉS en jocs que dibuixen per programari (no els 3D normals).",
   "Plus de FPS SEULEMENT dans les jeux rendus en logiciel (pas la 3D normale).",
   "Mehr FPS NUR in Spielen mit Software-Rendering (nicht normales 3D).",
   "Più FPS SOLO nei giochi disegnati via software (non il 3D normale).",
   "Mais FPS SÓ em jogos desenhados por software (não o 3D normal).")
TR(kSetHelp2c,
   "Downside: more pixelated image. In game, L+R toggles the same thing.",
   "Contra: imagen más pixelada. En partida, L+R cambia lo mismo.",
   "Contra: imatge més pixelada. En partida, L+R canvia el mateix.",
   "Inconvénient : image plus pixelisée. En jeu, L+R fait la même chose.",
   "Nachteil: pixeligeres Bild. Im Spiel schaltet L+R dasselbe um.",
   "Contro: immagine più pixelata. In gioco, L+R cambia la stessa cosa.",
   "Contra: imagem mais pixelada. No jogo, L+R muda o mesmo.")
TR(kSetHelp3a,
   "ON: the game's registers stay in the Vita's processor.",
   "ON: los registros del juego se quedan en el procesador de la Vita.",
   "ON: els registres del joc es queden al processador de la Vita.",
   "ON : les registres du jeu restent dans le processeur de la Vita.",
   "ON: die Register des Spiels bleiben im Prozessor der Vita.",
   "ON: i registri del gioco restano nel processore della Vita.",
   "ON: os registradores do jogo ficam no processador do Vita.")
TR(kSetHelp3b,
   "Gain: game CPU ~10-20% faster (several FPS in heavy games).",
   "Ganancia: CPU del juego ~10-20% más rápida (varios FPS en juegos pesados).",
   "Guany: CPU del joc ~10-20% més ràpida (uns quants FPS en jocs pesats).",
   "Gain : CPU du jeu ~10-20% plus rapide (plusieurs FPS dans les jeux lourds).",
   "Gewinn: Spiel-CPU ~10-20% schneller (einige FPS in schweren Spielen).",
   "Guadagno: CPU del gioco ~10-20% più veloce (vari FPS nei giochi pesanti).",
   "Ganho: CPU do jogo ~10-20% mais rápida (vários FPS em jogos pesados).")
TR(kSetHelp3c,
   "Downside: none known. Turn off only if a game freezes.",
   "Contra: ninguno conocido. Apagar solo si un juego se cuelga.",
   "Contra: cap de conegut. Apaga-ho només si un joc es penja.",
   "Inconvénient : aucun connu. À couper seulement si un jeu se fige.",
   "Nachteil: keiner bekannt. Nur ausschalten, wenn ein Spiel hängt.",
   "Contro: nessuno noto. Disattiva solo se un gioco si blocca.",
   "Contra: nenhum conhecido. Desligue só se um jogo travar.")
TR(kSetHelp4a,
   "ON: the game's floating point runs on the Vita's hardware.",
   "ON: la coma flotante del juego va por el hardware de la Vita.",
   "ON: la coma flotant del joc va pel maquinari de la Vita.",
   "ON : la virgule flottante du jeu passe par le matériel de la Vita.",
   "ON: das Gleitkomma des Spiels läuft auf der Hardware der Vita.",
   "ON: la virgola mobile del gioco usa l'hardware della Vita.",
   "ON: o ponto flutuante do jogo roda no hardware do Vita.")
TR(kSetHelp4b,
   "Gain: the biggest of the JIT in 3D games (about +10-25% FPS).",
   "Ganancia: la mayor del JIT en juegos 3D (del orden de +10-25% de FPS).",
   "Guany: el més gran del JIT en jocs 3D (de l'ordre de +10-25% de FPS).",
   "Gain : le plus gros du JIT dans les jeux 3D (environ +10-25% de FPS).",
   "Gewinn: der größte des JIT in 3D-Spielen (etwa +10-25% FPS).",
   "Guadagno: il maggiore del JIT nei giochi 3D (circa +10-25% di FPS).",
   "Ganho: o maior do JIT em jogos 3D (cerca de +10-25% de FPS).")
TR(kSetHelp4c,
   "Downside: if a game computes odd things (physics, animations), turn off.",
   "Contra: si un juego calcula cosas raras (físicas, animaciones), apagar.",
   "Contra: si un joc calcula coses estranyes (física, animacions), apaga-ho.",
   "Inconvénient : si un jeu calcule bizarrement (physique, animations), couper.",
   "Nachteil: rechnet ein Spiel seltsam (Physik, Animationen), ausschalten.",
   "Contro: se un gioco calcola cose strane (fisica, animazioni), disattiva.",
   "Contra: se um jogo calcular coisas estranhas (física, animações), desligue.")
TR(kSetHelp5a,
   "ON: the CPU doesn't stop to wait for the GPU when closing each scene.",
   "ON: no para la CPU a esperar a la GPU al cerrar cada escena.",
   "ON: no atura la CPU per esperar la GPU en tancar cada escena.",
   "ON : le CPU n'attend pas le GPU à la fin de chaque scène.",
   "ON: die CPU wartet am Ende jeder Szene nicht auf die GPU.",
   "ON: la CPU non aspetta la GPU alla chiusura di ogni scena.",
   "ON: a CPU não para esperando a GPU ao fechar cada cena.")
TR(kSetHelp5b, "Gain: ~3-7% FPS in 3D.", "Ganancia: ~3-7% de FPS en 3D.",
   "Guany: ~3-7% de FPS en 3D.", "Gain : ~3-7% de FPS en 3D.", "Gewinn: ~3-7% FPS in 3D.",
   "Guadagno: ~3-7% di FPS in 3D.", "Ganho: ~3-7% de FPS em 3D.")
TR(kSetHelp5c,
   "Downside: if the image freezes or flickers, turn off.",
   "Contra: si la imagen se congela o parpadea, apagar.",
   "Contra: si la imatge es congela o parpelleja, apaga-ho.",
   "Inconvénient : si l'image se fige ou clignote, couper.",
   "Nachteil: friert das Bild ein oder flackert es, ausschalten.",
   "Contro: se l'immagine si blocca o sfarfalla, disattiva.",
   "Contra: se a imagem congelar ou piscar, desligue.")
TR(kSetHelp6a,
   "ON: the image drawn by the GPU is shown without copying it twice.",
   "ON: la imagen dibujada por la GPU se muestra sin copiarla dos veces.",
   "ON: la imatge dibuixada per la GPU es mostra sense copiar-la dues vegades.",
   "ON : l'image du GPU s'affiche sans être copiée deux fois.",
   "ON: das GPU-Bild wird angezeigt, ohne es doppelt zu kopieren.",
   "ON: l'immagine della GPU si mostra senza copiarla due volte.",
   "ON: a imagem da GPU é mostrada sem ser copiada duas vezes.")
TR(kSetHelp6b,
   "Gain: ~2-5% FPS when the game allows it.",
   "Ganancia: ~2-5% de FPS cuando el juego lo permite.",
   "Guany: ~2-5% de FPS quan el joc ho permet.",
   "Gain : ~2-5% de FPS quand le jeu le permet.",
   "Gewinn: ~2-5% FPS, wenn das Spiel es erlaubt.",
   "Guadagno: ~2-5% di FPS quando il gioco lo permette.",
   "Ganho: ~2-5% de FPS quando o jogo permite.")
TR(kSetHelp6c,
   "Downside: if the image is rotated or has odd colors, turn off.",
   "Contra: si la imagen sale girada o con colores raros, apagar.",
   "Contra: si la imatge surt girada o amb colors estranys, apaga-ho.",
   "Inconvénient : image tournée ou couleurs bizarres ? Couper.",
   "Nachteil: ist das Bild gedreht oder farblich falsch, ausschalten.",
   "Contro: se l'immagine è ruotata o ha colori strani, disattiva.",
   "Contra: se a imagem sair girada ou com cores estranhas, desligue.")
TR(kSetHelp7a,
   "ON: the emulated CPU jumps from block to block without returning to the emulator.",
   "ON: la CPU emulada salta de bloque en bloque sin volver al emulador.",
   "ON: la CPU emulada salta de bloc en bloc sense tornar a l'emulador.",
   "ON : le CPU émulé passe de bloc en bloc sans revenir à l'émulateur.",
   "ON: die emulierte CPU springt von Block zu Block ohne Umweg über den Emulator.",
   "ON: la CPU emulata salta da blocco a blocco senza tornare all'emulatore.",
   "ON: a CPU emulada salta de bloco em bloco sem voltar ao emulador.")
TR(kSetHelp7b, "Gain: ~5-15% FPS.", "Ganancia: ~5-15% de FPS.", "Guany: ~5-15% de FPS.",
   "Gain : ~5-15% de FPS.", "Gewinn: ~5-15% FPS.", "Guadagno: ~5-15% di FPS.",
   "Ganho: ~5-15% de FPS.")
TR(kSetHelp7c,
   "Downside: none known. Turn off if there are unexplained freezes.",
   "Contra: ninguno conocido. Apagar si hay cuelgues sin explicación.",
   "Contra: cap de conegut. Apaga-ho si hi ha penjades sense explicació.",
   "Inconvénient : aucun connu. Couper en cas de blocages inexpliqués.",
   "Nachteil: keiner bekannt. Bei unerklärlichen Hängern ausschalten.",
   "Contro: nessuno noto. Disattiva se ci sono blocchi inspiegabili.",
   "Contra: nenhum conhecido. Desligue se houver travamentos sem motivo.")
TR(kSetHelp8a,
   "ON: vertex shaders with complex branches run on the GPU.",
   "ON: los shaders de vértices con saltos complicados van a la GPU.",
   "ON: els shaders de vèrtexs amb salts complicats van a la GPU.",
   "ON : les shaders de sommets à sauts complexes passent sur le GPU.",
   "ON: Vertex-Shader mit komplexen Sprüngen laufen auf der GPU.",
   "ON: gli shader dei vertici con salti complessi vanno sulla GPU.",
   "ON: shaders de vértices com saltos complexos vão para a GPU.")
TR(kSetHelp8b,
   "Gain: the biggest in 3D (+30% or more if the compiler accepts them).",
   "Ganancia: la mayor en 3D (+30% o más si el compilador los acepta).",
   "Guany: el més gran en 3D (+30% o més si el compilador els accepta).",
   "Gain : le plus gros en 3D (+30% ou plus si le compilateur les accepte).",
   "Gewinn: der größte in 3D (+30% oder mehr, wenn der Compiler sie annimmt).",
   "Guadagno: il maggiore in 3D (+30% o più se il compilatore li accetta).",
   "Ganho: o maior em 3D (+30% ou mais se o compilador os aceitar).")
TR(kSetHelp8c,
   "Downside: the 1st time a scene takes longer to load. If 3D looks broken, turn off.",
   "Contra: la 1a vez tarda más en cargar la escena. Si el 3D sale roto, apagar.",
   "Contra: la 1a vegada triga més a carregar l'escena. Si el 3D surt trencat, apaga-ho.",
   "Inconvénient : la 1re fois, la scène charge plus lentement. 3D cassée ? Couper.",
   "Nachteil: beim 1. Mal lädt die Szene länger. Ist das 3D kaputt, ausschalten.",
   "Contro: la 1a volta la scena carica più lentamente. Se il 3D è rotto, disattiva.",
   "Contra: na 1a vez a cena demora mais para carregar. Se o 3D quebrar, desligue.")
TR(kSetHelp9a,
   "ON: each repeated vertex is computed only once per draw.",
   "ON: cada vértice repetido se calcula una sola vez por dibujo.",
   "ON: cada vèrtex repetit es calcula una sola vegada per dibuix.",
   "ON : chaque sommet répété n'est calculé qu'une fois par dessin.",
   "ON: jeder wiederholte Vertex wird pro Zeichnung nur einmal berechnet.",
   "ON: ogni vertice ripetuto si calcola una sola volta per disegno.",
   "ON: cada vértice repetido é calculado uma só vez por desenho.")
TR(kSetHelp9b,
   "Gain: 0-15% in 3D, depending on the game.",
   "Ganancia: 0-15% en 3D, según el juego.",
   "Guany: 0-15% en 3D, segons el joc.",
   "Gain : 0-15% en 3D, selon le jeu.",
   "Gewinn: 0-15% in 3D, je nach Spiel.",
   "Guadagno: 0-15% in 3D, a seconda del gioco.",
   "Ganho: 0-15% em 3D, dependendo do jogo.")
TR(kSetHelp9c,
   "Downside: if a 3D model shows spikes or is deformed, turn off.",
   "Contra: si un modelo 3D sale con picos o deformado, apagar.",
   "Contra: si un model 3D surt amb punxes o deformat, apaga-ho.",
   "Inconvénient : modèle 3D avec des pics ou déformé ? Couper.",
   "Nachteil: zeigt ein 3D-Modell Spitzen oder Verformungen, ausschalten.",
   "Contro: se un modello 3D ha punte o è deformato, disattiva.",
   "Contra: se um modelo 3D aparecer com picos ou deformado, desligue.")
TR(kSetHelp10a,
   "OFF: no sound. Saves the audio stretching and output.",
   "OFF: sin sonido. Se ahorra el estirado y el envío del audio.",
   "OFF: sense so. S'estalvia l'estirament i l'enviament de l'àudio.",
   "OFF : pas de son. Économise l'étirement et l'envoi de l'audio.",
   "OFF: kein Ton. Spart Audio-Dehnung und -Ausgabe.",
   "OFF: niente audio. Si risparmia lo stretching e l'invio dell'audio.",
   "OFF: sem som. Economiza o estiramento e o envio do áudio.")
TR(kSetHelp10b,
   "Gain: ~2-5% FPS. Applied when the game starts.",
   "Ganancia: ~2-5% de FPS. Se aplica al arrancar el juego.",
   "Guany: ~2-5% de FPS. S'aplica en arrencar el joc.",
   "Gain : ~2-5% de FPS. Appliqué au lancement du jeu.",
   "Gewinn: ~2-5% FPS. Gilt beim Spielstart.",
   "Guadagno: ~2-5% di FPS. Si applica all'avvio del gioco.",
   "Ganho: ~2-5% de FPS. Aplicado ao iniciar o jogo.")
TR(kSetHelp10c, "Downside: no sound.", "Contra: no hay sonido.", "Contra: no hi ha so.",
   "Inconvénient : pas de son.", "Nachteil: kein Ton.", "Contro: niente audio.",
   "Contra: sem som.")
TR(kSetHelp11a,
   "How the screens are arranged. The Vita's GPU does the scaling.",
   "Cómo se colocan las pantallas. La escala la hace la GPU de la Vita.",
   "Com es col·loquen les pantalles. L'escala la fa la GPU de la Vita.",
   "Disposition des écrans. La mise à l'échelle est faite par le GPU de la Vita.",
   "Anordnung der Bildschirme. Die Skalierung macht die GPU der Vita.",
   "Come si dispongono gli schermi. La scala la fa la GPU della Vita.",
   "Como as telas são dispostas. A escala é feita pela GPU do Vita.")
TR(kSetHelp11b,
   "Does not change FPS. With only the top screen, the touch screen can't be seen or used.",
   "No cambia los FPS. Con solo la superior no se ve la táctil ni se puede tocar.",
   "No canvia els FPS. Amb només la superior no es veu la tàctil ni es pot tocar.",
   "Ne change pas les FPS. Avec l'écran du haut seul, le tactile est invisible.",
   "Ändert die FPS nicht. Nur oben: Touchscreen unsichtbar und nicht nutzbar.",
   "Non cambia gli FPS. Con solo il superiore il touch non si vede né si usa.",
   "Não muda o FPS. Só com a superior, a tela de toque some e não funciona.")
TR(kSetHelp11c,
   "Applied immediately, also in game.",
   "Se aplica al momento, también en partida.",
   "S'aplica a l'instant, també en partida.",
   "Appliqué immédiatement, aussi en jeu.",
   "Gilt sofort, auch im Spiel.",
   "Si applica subito, anche in gioco.",
   "Aplicado na hora, também no jogo.")
TR(kSetHelp12a,
   "ON: if the game hasn't drawn anything new, it isn't shown again.",
   "ON: si el juego no ha dibujado nada nuevo, no se vuelve a mostrar.",
   "ON: si el joc no ha dibuixat res de nou, no es torna a mostrar.",
   "ON : si le jeu n'a rien dessiné de neuf, rien n'est réaffiché.",
   "ON: hat das Spiel nichts Neues gezeichnet, wird nichts erneut angezeigt.",
   "ON: se il gioco non ha disegnato nulla di nuovo, non si mostra di nuovo.",
   "ON: se o jogo não desenhou nada novo, não é mostrado de novo.")
TR(kSetHelp12b,
   "Gain: ~5-10% FPS in games at 30 FPS or less.",
   "Ganancia: ~5-10% de FPS en juegos a 30 FPS o menos.",
   "Guany: ~5-10% de FPS en jocs a 30 FPS o menys.",
   "Gain : ~5-10% de FPS dans les jeux à 30 FPS ou moins.",
   "Gewinn: ~5-10% FPS in Spielen mit 30 FPS oder weniger.",
   "Guadagno: ~5-10% di FPS nei giochi a 30 FPS o meno.",
   "Ganho: ~5-10% de FPS em jogos a 30 FPS ou menos.")
TR(kSetHelp12c,
   "Downside: if a game leaves the image frozen, turn off.",
   "Contra: si un juego deja la imagen congelada, apagar.",
   "Contra: si un joc deixa la imatge congelada, apaga-ho.",
   "Inconvénient : si un jeu laisse l'image figée, couper.",
   "Nachteil: bleibt das Bild in einem Spiel stehen, ausschalten.",
   "Contro: se un gioco lascia l'immagine bloccata, disattiva.",
   "Contra: se um jogo deixar a imagem congelada, desligue.")
TR(kSetHelp13a,
   "ON: shaders with branches are compiled once per combination of options.",
   "ON: los shaders con saltos se compilan una vez por combinación de opciones.",
   "ON: els shaders amb salts es compilen un cop per combinació d'opcions.",
   "ON : les shaders à sauts sont compilés une fois par combinaison d'options.",
   "ON: Shader mit Sprüngen werden je Optionskombination einmal kompiliert.",
   "ON: gli shader con salti si compilano una volta per combinazione di opzioni.",
   "ON: shaders com saltos são compilados uma vez por combinação de opções.")
TR(kSetHelp13b,
   "Gain: the biggest in 3D (Zafiro Alfa: from ~2.5 FPS with OFF to several times more).",
   "Ganancia: la mayor en 3D (Zafiro Alfa: de ~2.5 FPS con OFF a varias veces más).",
   "Guany: el més gran en 3D (Zafiro Alfa: de ~2.5 FPS amb OFF a diverses vegades més).",
   "Gain : le plus gros en 3D (Zafiro Alfa : de ~2.5 FPS en OFF à plusieurs fois plus).",
   "Gewinn: der größte in 3D (Zafiro Alfa: von ~2.5 FPS bei OFF auf ein Vielfaches).",
   "Guadagno: il maggiore in 3D (Zafiro Alfa: da ~2.5 FPS con OFF a varie volte tanto).",
   "Ganho: o maior em 3D (Zafiro Alfa: de ~2.5 FPS com OFF a várias vezes mais).")
TR(kSetHelp13c,
   "Downside: the 1st time each one appears, the scene stops a few seconds. If it crashes, turn off.",
   "Contra: la 1a vez que sale cada uno, la escena se para unos segundos. Si crashea, apagar.",
   "Contra: la 1a vegada que surt cadascun, l'escena s'atura uns segons. Si peta, apaga-ho.",
   "Inconvénient : à la 1re apparition, la scène s'arrête quelques secondes. Plantage ? Couper.",
   "Nachteil: beim 1. Auftreten stockt die Szene ein paar Sekunden. Bei Absturz ausschalten.",
   "Contro: la 1a volta che appare, la scena si ferma qualche secondo. Se crasha, disattiva.",
   "Contra: na 1a vez que cada um aparece, a cena para alguns segundos. Se travar, desligue.")
TR(kSetHelp14a,
   "ON: screen copies and clears on the GPU, without a round trip through the CPU.",
   "ON: copia de pantalla y borrados en la GPU, sin bajar a la CPU y volver a subir.",
   "ON: còpia de pantalla i esborrats a la GPU, sense baixar a la CPU i tornar a pujar.",
   "ON : copies d'écran et effacements sur le GPU, sans aller-retour par le CPU.",
   "ON: Bildschirmkopien und Löschen auf der GPU, ohne Umweg über die CPU.",
   "ON: copie dello schermo e pulizie sulla GPU, senza passare dalla CPU.",
   "ON: cópia de tela e limpezas na GPU, sem ida e volta pela CPU.")
TR(kSetHelp14b,
   "Gain: Zafiro Alfa's 2D went from 11 to 22 FPS; in 3D, several ms per frame.",
   "Ganancia: el 2D de Zafiro Alfa pasó de 11 a 22 FPS; en 3D, varios ms por fotograma.",
   "Guany: el 2D de Zafiro Alfa va passar d'11 a 22 FPS; en 3D, uns quants ms per fotograma.",
   "Gain : la 2D de Zafiro Alfa est passée de 11 à 22 FPS ; en 3D, plusieurs ms par image.",
   "Gewinn: Zafiro Alfa in 2D stieg von 11 auf 22 FPS; in 3D einige ms pro Bild.",
   "Guadagno: il 2D di Zafiro Alfa è passato da 11 a 22 FPS; in 3D, vari ms per fotogramma.",
   "Ganho: o 2D de Zafiro Alfa foi de 11 para 22 FPS; em 3D, vários ms por quadro.")
TR(kSetHelp14c,
   "Downside: if you see old images, odd colors or flicker, turn off.",
   "Contra: si ves imágenes viejas, colores raros o parpadeos, apagar.",
   "Contra: si veus imatges velles, colors estranys o parpelleigs, apaga-ho.",
   "Inconvénient : images anciennes, couleurs bizarres ou scintillement ? Couper.",
   "Nachteil: bei alten Bildern, falschen Farben oder Flackern ausschalten.",
   "Contro: se vedi immagini vecchie, colori strani o sfarfallii, disattiva.",
   "Contra: se aparecerem imagens antigas, cores estranhas ou piscadas, desligue.")
TR(kSetHelp15a,
   "ON: the emulated GPU and the image run on core 1; the game alone on core 0.",
   "ON: la GPU emulada y la imagen van en el núcleo 1; el juego, solo en el 0.",
   "ON: la GPU emulada i la imatge van al nucli 1; el joc, sol al 0.",
   "ON : le GPU émulé et l'image tournent sur le coeur 1 ; le jeu seul sur le 0.",
   "ON: emulierte GPU und Bild laufen auf Kern 1; das Spiel allein auf Kern 0.",
   "ON: la GPU emulata e l'immagine vanno sul core 1; il gioco da solo sullo 0.",
   "ON: a GPU emulada e a imagem rodam no núcleo 1; o jogo sozinho no 0.")
TR(kSetHelp15b,
   "Gain: whatever the GPU takes per frame (estimate: +20-50% where it matters). On game start.",
   "Ganancia: lo que tarde la GPU por fotograma (cálculo: +20-50% donde pesa). Al arrancar juego.",
   "Guany: el que trigui la GPU per fotograma (càlcul: +20-50% on pesa). En arrencar el joc.",
   "Gain : le temps GPU par image (estimation : +20-50% là où il pèse). Au lancement du jeu.",
   "Gewinn: die GPU-Zeit pro Bild (Schätzung: +20-50%, wo sie zählt). Beim Spielstart.",
   "Guadagno: il tempo della GPU per fotogramma (stima: +20-50% dove pesa). All'avvio del gioco.",
   "Ganho: o tempo da GPU por quadro (estimativa: +20-50% onde pesa). Ao iniciar o jogo.")
TR(kSetHelp15c,
   "Downside: new. If a game freezes, stutters or looks odd, turn off.",
   "Contra: nuevo. Si un juego se cuelga, va a saltos o se ve raro, apagar.",
   "Contra: nou. Si un joc es penja, va a salts o es veu estrany, apaga-ho.",
   "Inconvénient : nouveau. Si un jeu se fige, saccade ou s'affiche mal, couper.",
   "Nachteil: neu. Hängt, ruckelt oder sieht ein Spiel seltsam aus, ausschalten.",
   "Contro: nuovo. Se un gioco si blocca, scatta o si vede strano, disattiva.",
   "Contra: novo. Se um jogo travar, engasgar ou ficar estranho, desligue.")
TR(kSetHelp16a,
   "Unlimited: doesn't wait for the screen refresh; can go above 100% speed.",
   "Sin límite: no espera al refresco de la pantalla; puede pasar del 100% de velocidad.",
   "Sense límit: no espera el refresc de la pantalla; pot passar del 100% de velocitat.",
   "Illimitée : n'attend pas le rafraîchissement ; peut dépasser 100% de vitesse.",
   "Unbegrenzt: wartet nicht auf die Bildwiederholung; kann über 100% laufen.",
   "Illimitata: non aspetta il refresh dello schermo; può superare il 100%.",
   "Sem limite: não espera a atualização da tela; pode passar de 100% de velocidade.")
TR(kSetHelp16b,
   "Gain: menus and light scenes run faster than on the console.",
   "Ganancia: menús y escenas ligeras van más rápido que en la consola.",
   "Guany: menús i escenes lleugeres van més ràpid que a la consola.",
   "Gain : menus et scènes légères vont plus vite que sur la console.",
   "Gewinn: Menüs und leichte Szenen laufen schneller als auf der Konsole.",
   "Guadagno: menu e scene leggere vanno più veloci che sulla console.",
   "Ganho: menus e cenas leves rodam mais rápido que no console.")
TR(kSetHelp16c,
   "Downside: the game may run sped up and the sound stretched. Applied immediately.",
   "Contra: el juego puede ir acelerado y el sonido estirado. Se aplica al momento.",
   "Contra: el joc pot anar accelerat i el so estirat. S'aplica a l'instant.",
   "Inconvénient : le jeu peut s'accélérer et le son s'étirer. Immédiat.",
   "Nachteil: Spiel evtl. zu schnell, Ton gedehnt. Gilt sofort.",
   "Contro: il gioco può andare accelerato e l'audio stirato. Si applica subito.",
   "Contra: o jogo pode acelerar e o som esticar. Aplicado na hora.")
TR(kSetHelp17a,
   "0.5x: the GPU draws half per axis (less sharp). 2x: double (sharper).",
   "0.5x: la GPU dibuja la mitad por eje (menos nítido). 2x: el doble (más nítido).",
   "0.5x: la GPU dibuixa la meitat per eix (menys nítid). 2x: el doble (més nítid).",
   "0.5x : le GPU dessine moitié moins par axe (moins net). 2x : le double (plus net).",
   "0.5x: die GPU zeichnet je Achse die Hälfte (unschärfer). 2x: doppelt (schärfer).",
   "0.5x: la GPU disegna metà per asse (meno nitido). 2x: il doppio (più nitido).",
   "0.5x: a GPU desenha metade por eixo (menos nítido). 2x: o dobro (mais nítido).")
TR(kSetHelp17b,
   "0.5x relieves the Vita's GPU; 2x costs GPU and video memory. On game start.",
   "0.5x alivia la GPU de la Vita; 2x cuesta GPU y memoria de vídeo. Al arrancar el juego.",
   "0.5x alleuja la GPU de la Vita; 2x costa GPU i memòria de vídeo. En arrencar el joc.",
   "0.5x soulage le GPU de la Vita ; 2x coûte GPU et mémoire vidéo. Au lancement.",
   "0.5x entlastet die Vita-GPU; 2x kostet GPU und Videospeicher. Beim Spielstart.",
   "0.5x alleggerisce la GPU della Vita; 2x costa GPU e memoria video. All'avvio.",
   "0.5x alivia a GPU do Vita; 2x custa GPU e memória de vídeo. Ao iniciar o jogo.")
TR(kSetHelp17c,
   "Downside: experimental. Software-drawn parts stay at 1x. If slow or odd, use 1x.",
   "Contra: experimental. Lo que vaya por software sigue a 1x. Si va lento o raro, 1x.",
   "Contra: experimental. El que vagi per programari segueix a 1x. Si va lent o estrany, 1x.",
   "Inconvénient : expérimental. Le rendu logiciel reste en 1x. Lent ou bizarre ? 1x.",
   "Nachteil: experimentell. Software-Rendering bleibt 1x. Langsam oder seltsam? 1x.",
   "Contro: sperimentale. Ciò che va via software resta a 1x. Se lento o strano, 1x.",
   "Contra: experimental. O que for por software fica em 1x. Se lento ou estranho, 1x.")
TR(kSetHelp18a,
   "ON: while a model's vertex shader compiles, that model isn't drawn.",
   "ON: mientras se compila el shader de vértices de un modelo, ese modelo no se dibuja.",
   "ON: mentre es compila el shader de vèrtexs d'un model, aquest model no es dibuixa.",
   "ON : pendant la compilation du shader d'un modèle, ce modèle n'est pas dessiné.",
   "ON: während der Vertex-Shader eines Modells kompiliert, wird es nicht gezeichnet.",
   "ON: mentre si compila lo shader dei vertici di un modello, quel modello non si vede.",
   "ON: enquanto o shader de vértices de um modelo compila, ele não é desenhado.")
TR(kSetHelp18b,
   "Gain: the 1st time you enter a 3D scene it runs smoothly instead of at 2-3 FPS.",
   "Ganancia: la 1a vez que se entra a una escena 3D va fluida en vez de a 2-3 FPS.",
   "Guany: la 1a vegada que s'entra a una escena 3D va fluida en lloc d'anar a 2-3 FPS.",
   "Gain : la 1re fois dans une scène 3D, c'est fluide au lieu de 2-3 FPS.",
   "Gewinn: beim 1. Betreten einer 3D-Szene flüssig statt mit 2-3 FPS.",
   "Guadagno: la 1a volta in una scena 3D è fluida invece che a 2-3 FPS.",
   "Ganho: na 1a vez em uma cena 3D fica fluido em vez de 2-3 FPS.")
TR(kSetHelp18c,
   "Downside: models appear a few seconds later. OFF: shown at once, but choppy.",
   "Contra: los modelos aparecen unos segundos después. OFF: se ven ya, pero a saltos.",
   "Contra: els models apareixen uns segons després. OFF: es veuen de seguida, però a salts.",
   "Inconvénient : les modèles apparaissent quelques secondes après. OFF : tout de suite, mais saccadé.",
   "Nachteil: Modelle erscheinen Sekunden später. OFF: sofort sichtbar, aber ruckelig.",
   "Contro: i modelli appaiono qualche secondo dopo. OFF: subito visibili, ma a scatti.",
   "Contra: os modelos aparecem segundos depois. OFF: aparecem já, mas engasgando.")
TR(kSetHelp19a,
   "GXM: the Vita's graphics chip draws the game. Software: the CPU draws everything.",
   "GXM: el chip gráfico de la Vita dibuja el juego. Software: la CPU lo dibuja todo.",
   "GXM: el xip gràfic de la Vita dibuixa el joc. Programari: la CPU ho dibuixa tot.",
   "GXM : la puce graphique de la Vita dessine le jeu. Logiciel : le CPU dessine tout.",
   "GXM: der Grafikchip der Vita zeichnet das Spiel. Software: die CPU zeichnet alles.",
   "GXM: il chip grafico della Vita disegna il gioco. Software: la CPU disegna tutto.",
   "GXM: o chip gráfico do Vita desenha o jogo. Software: a CPU desenha tudo.")
TR(kSetHelp19b,
   "GXM is much faster. Software only for testing, or if a game looks wrong with GXM.",
   "GXM es mucho más rápido. Software solo para pruebas o si un juego se ve mal con GXM.",
   "GXM és molt més ràpid. Programari només per a proves o si un joc es veu malament amb GXM.",
   "GXM est bien plus rapide. Logiciel seulement pour tester, ou si un jeu s'affiche mal.",
   "GXM ist viel schneller. Software nur zum Testen oder wenn ein Spiel mit GXM falsch aussieht.",
   "GXM è molto più veloce. Software solo per prove o se un gioco si vede male con GXM.",
   "GXM é muito mais rápido. Software só para testes ou se um jogo ficar errado com GXM.")
TR(kSetHelp19c,
   "Applied when the game starts. Not saved: back to GXM when the app restarts.",
   "Se aplica al arrancar el juego. No se guarda: vuelve a GXM al reabrir la app.",
   "S'aplica en arrencar el joc. No es desa: torna a GXM en reobrir l'app.",
   "Appliqué au lancement du jeu. Non enregistré : retour à GXM au redémarrage.",
   "Gilt beim Spielstart. Wird nicht gespeichert: nach Neustart wieder GXM.",
   "Si applica all'avvio del gioco. Non si salva: torna a GXM riaprendo l'app.",
   "Aplicado ao iniciar o jogo. Não é salvo: volta ao GXM ao reabrir o app.")

// Valores de algunas filas.
TR(kValUnlimited, "Unlimited", "Sin límite", "Sense límit", "Illimitée", "Unbegrenzt",
   "Illimitata", "Sem limite")
TR(kValRefresh, "100% (refresh rate)", "100% (refresco)", "100% (refresc)",
   "100% (rafraîchissement)", "100% (Bildrate)", "100% (refresh)", "100% (atualização)")
TR(kVal2x, "2x (experimental)", "2x (experimental)", "2x (experimental)", "2x (expérimental)",
   "2x (experimentell)", "2x (sperimentale)", "2x (experimental)")
TR(kValSoftware, "Software", "Software", "Programari", "Logiciel", "Software", "Software",
   "Software")
TR(kLayout0, "Normal (1:1)", "Normal (1:1)", "Normal (1:1)", "Normal (1:1)", "Normal (1:1)",
   "Normale (1:1)", "Normal (1:1)")
TR(kLayout1, "Large top + bottom", "Superior grande + inferior", "Superior gran + inferior",
   "Haut agrandi + bas", "Oben groß + unten", "Superiore grande + inferiore",
   "Superior grande + inferior")
TR(kLayout2, "Side by side", "Lado a lado", "Costat a costat", "Côte à côte", "Nebeneinander",
   "Affiancati", "Lado a lado")
TR(kLayout3, "Top only x2", "Solo superior x2", "Només superior x2", "Haut seul x2",
   "Nur oben x2", "Solo superiore x2", "Só superior x2")
TR(kLayout4, "Top only, full screen", "Solo superior, pantalla completa",
   "Només superior, pantalla completa", "Haut seul, plein écran", "Nur oben, Vollbild",
   "Solo superiore, schermo intero", "Só superior, tela cheia")

// El idioma que ve el juego (fila 1 de los ajustes), en el orden de kLanguages.
TR(kSysLangAuto, "Automatic (emulated console's)", "Automático (el de la consola emulada)",
   "Automàtic (el de la consola emulada)", "Automatique (celle de la console)",
   "Automatisch (der emulierten Konsole)", "Automatica (quella della console)",
   "Automático (o do console emulado)")
TR(kSysLangEs, "Spanish", "Español", "Castellà", "Espagnol", "Spanisch", "Spagnolo", "Espanhol")
TR(kSysLangEn, "English", "Inglés", "Anglès", "Anglais", "Englisch", "Inglese", "Inglês")
TR(kSysLangFr, "French", "Francés", "Francès", "Français", "Französisch", "Francese", "Francês")
TR(kSysLangDe, "German", "Alemán", "Alemany", "Allemand", "Deutsch", "Tedesco", "Alemão")
TR(kSysLangIt, "Italian", "Italiano", "Italià", "Italien", "Italienisch", "Italiano", "Italiano")
TR(kSysLangPt, "Portuguese", "Portugués", "Portuguès", "Portugais", "Portugiesisch",
   "Portoghese", "Português")
TR(kSysLangNl, "Dutch", "Neerlandés", "Neerlandès", "Néerlandais", "Niederländisch", "Olandese",
   "Holandês")
TR(kSysLangRu, "Russian", "Ruso", "Rus", "Russe", "Russisch", "Russo", "Russo")
TR(kSysLangJa, "Japanese", "Japonés", "Japonès", "Japonais", "Japanisch", "Giapponese", "Japonês")
TR(kSysLangZh, "Simplified Chinese", "Chino simplificado", "Xinès simplificat",
   "Chinois simplifié", "Chinesisch (vereinfacht)", "Cinese semplificato", "Chinês simplificado")
TR(kSysLangTw, "Traditional Chinese", "Chino tradicional", "Xinès tradicional",
   "Chinois traditionnel", "Chinesisch (traditionell)", "Cinese tradizionale",
   "Chinês tradicional")
TR(kSysLangKo, "Korean", "Coreano", "Coreà", "Coréen", "Koreanisch", "Coreano", "Coreano")

// --- Carga y partida ---------------------------------------------------------
TR(kLoading, "Loading...", "Cargando...", "Carregant...", "Chargement...", "Wird geladen...",
   "Caricamento...", "Carregando...")
TR(kLoadingShaders, "Loading shaders...", "Cargando shaders...", "Carregant shaders...",
   "Chargement des shaders...", "Shader werden geladen...", "Caricamento shader...",
   "Carregando shaders...")
TR(kShaderCount, "%u / %u shaders", "%u / %u shaders", "%u / %u shaders", "%u / %u shaders",
   "%u / %u Shader", "%u / %u shader", "%u / %u shaders")
TR(kNoShaders1,
   "No shaders saved for this game yet: they will be",
   "No hay shaders guardados de este juego todavía: se irán",
   "Encara no hi ha shaders desats d'aquest joc: s'aniran",
   "Pas encore de shaders enregistrés pour ce jeu : ils seront",
   "Für dieses Spiel sind noch keine Shader gespeichert: sie werden",
   "Nessuno shader salvato per questo gioco: verranno",
   "Ainda não há shaders salvos deste jogo: eles serão")
TR(kNoShaders2,
   "saved as you play and will load here next time.",
   "guardando al jugar y la próxima vez cargarán aquí.",
   "desant en jugar i la propera vegada es carregaran aquí.",
   "enregistrés en jouant et chargés ici la prochaine fois.",
   "beim Spielen gespeichert und nächstes Mal hier geladen.",
   "salvati giocando e la prossima volta si caricheranno qui.",
   "salvos enquanto você joga e carregarão aqui na próxima vez.")
TR(kEmulating, "Emulating...", "Emulando...", "Emulant...", "Émulation...", "Emulation...",
   "Emulazione...", "Emulando...")
TR(kNoFirstFrame,
   "The game hasn't drawn its first frame yet.",
   "El juego aún no ha dibujado su primer fotograma.",
   "El joc encara no ha dibuixat el primer fotograma.",
   "Le jeu n'a pas encore dessiné sa première image.",
   "Das Spiel hat sein erstes Bild noch nicht gezeichnet.",
   "Il gioco non ha ancora disegnato il primo fotogramma.",
   "O jogo ainda não desenhou o primeiro quadro.")
TR(kTimeCycles, "Time: %u s    Emulation cycles: %u", "Tiempo: %u s    Ciclos de emulación: %u",
   "Temps: %u s    Cicles d'emulació: %u", "Temps : %u s    Cycles d'émulation : %u",
   "Zeit: %u s    Emulationszyklen: %u", "Tempo: %u s    Cicli di emulazione: %u",
   "Tempo: %u s    Ciclos de emulação: %u")
TR(kFreeMemory, "Free memory: %d KB", "Memoria libre: %d KB", "Memòria lliure: %d KB",
   "Mémoire libre : %d Ko", "Freier Speicher: %d KB", "Memoria libera: %d KB",
   "Memória livre: %d KB")
TR(kThreadDied, "A thread died from an exception:", "Un hilo murió por una excepción:",
   "Un fil ha mort per una excepció:", "Un thread est mort sur une exception :",
   "Ein Thread starb an einer Ausnahme:", "Un thread è morto per un'eccezione:",
   "Uma thread morreu por uma exceção:")
TR(kExitCombo, "START + SELECT: back to menu", "START + SELECT: volver al menú",
   "START + SELECT: tornar al menú", "START + SELECT : retour au menu",
   "START + SELECT: zurück zum Menü", "START + SELECT: torna al menu",
   "START + SELECT: voltar ao menu")

// --- Errores -----------------------------------------------------------------
TR(kErrNoMemory, "Out of memory", "Sin memoria", "Sense memòria", "Mémoire insuffisante",
   "Kein Speicher", "Memoria esaurita", "Sem memória")
TR(kErrNoMemoryDetail,
   "There isn't enough memory to load this game. Try a smaller one, or rebuild with a larger AZAHAR_HEAP_MB.",
   "No hay memoria suficiente para cargar este juego. Prueba con uno más pequeño, o recompila ajustando AZAHAR_HEAP_MB.",
   "No hi ha prou memòria per carregar aquest joc. Prova'n un de més petit, o recompila ajustant AZAHAR_HEAP_MB.",
   "Pas assez de mémoire pour charger ce jeu. Essayez-en un plus petit, ou recompilez avec un AZAHAR_HEAP_MB plus grand.",
   "Nicht genug Speicher für dieses Spiel. Versuche ein kleineres oder kompiliere mit größerem AZAHAR_HEAP_MB.",
   "Memoria insufficiente per caricare questo gioco. Prova con uno più piccolo, o ricompila con AZAHAR_HEAP_MB più grande.",
   "Não há memória suficiente para carregar este jogo. Tente um menor, ou recompile com um AZAHAR_HEAP_MB maior.")
TR(kErrLoad, "Error loading", "Error al cargar", "Error en carregar", "Erreur de chargement",
   "Fehler beim Laden", "Errore di caricamento", "Erro ao carregar")
TR(kErrEmulation, "Error during emulation", "Error durante la emulación",
   "Error durant l'emulació", "Erreur pendant l'émulation", "Fehler während der Emulation",
   "Errore durante l'emulazione", "Erro durante a emulação")
TR(kErrEmulationMemory, "Out of memory during emulation", "Sin memoria durante la emulación",
   "Sense memòria durant l'emulació", "Mémoire insuffisante pendant l'émulation",
   "Kein Speicher während der Emulation", "Memoria esaurita durante l'emulazione",
   "Sem memória durante a emulação")
TR(kErrEmulationMemoryDetail,
   "The game loaded but ran out of memory while running.",
   "El juego cargó pero se agotó la memoria al ejecutarlo.",
   "El joc es va carregar però es va esgotar la memòria en executar-lo.",
   "Le jeu s'est chargé mais la mémoire s'est épuisée pendant l'exécution.",
   "Das Spiel wurde geladen, aber beim Ausführen ging der Speicher aus.",
   "Il gioco è stato caricato ma la memoria si è esaurita durante l'esecuzione.",
   "O jogo carregou, mas a memória acabou durante a execução.")
TR(kErrNonStandard, "Exception of a non-standard type.", "Excepción de un tipo no estándar.",
   "Excepció d'un tipus no estàndard.", "Exception d'un type non standard.",
   "Ausnahme eines nicht standardmäßigen Typs.", "Eccezione di un tipo non standard.",
   "Exceção de um tipo não padrão.")
TR(kErrFatalStart, "Fatal error at startup", "Error fatal al iniciar", "Error fatal en iniciar",
   "Erreur fatale au démarrage", "Schwerer Fehler beim Start", "Errore fatale all'avvio",
   "Erro fatal ao iniciar")
TR(kErrUnknownException, "Unknown exception type.", "Excepción de tipo desconocido.",
   "Excepció de tipus desconegut.", "Exception de type inconnu.", "Unbekannter Ausnahmetyp.",
   "Eccezione di tipo sconosciuto.", "Exceção de tipo desconhecido.")
TR(kErrUncaught, "Uncaught exception", "Excepción no capturada", "Excepció no capturada",
   "Exception non interceptée", "Nicht abgefangene Ausnahme", "Eccezione non gestita",
   "Exceção não capturada")
TR(kErrUncaughtFooter,
   "Logged in ux0:/data/azahar/crash.txt  -  closing in %d s",
   "Anotado en ux0:/data/azahar/crash.txt  -  cerrando en %d s",
   "Anotat a ux0:/data/azahar/crash.txt  -  es tanca en %d s",
   "Noté dans ux0:/data/azahar/crash.txt  -  fermeture dans %d s",
   "In ux0:/data/azahar/crash.txt vermerkt  -  schließt in %d s",
   "Annotato in ux0:/data/azahar/crash.txt  -  chiusura tra %d s",
   "Registrado em ux0:/data/azahar/crash.txt  -  fechando em %d s")
TR(kFreeUserMemory, "Free user memory: %d KB", "Memoria de usuario libre: %d KB",
   "Memòria d'usuari lliure: %d KB", "Mémoire utilisateur libre : %d Ko",
   "Freier Benutzerspeicher: %d KB", "Memoria utente libera: %d KB",
   "Memória de usuário livre: %d KB")
TR(kHeapInfo, "Heap: %u MB total, %d KB in use, %d KB free inside",
   "Heap: %u MB totales, %d KB en uso, %d KB libres dentro",
   "Heap: %u MB totals, %d KB en ús, %d KB lliures a dins",
   "Tas : %u Mo au total, %d Ko utilisés, %d Ko libres dedans",
   "Heap: %u MB gesamt, %d KB belegt, %d KB frei darin",
   "Heap: %u MB totali, %d KB in uso, %d KB liberi dentro",
   "Heap: %u MB no total, %d KB em uso, %d KB livres dentro")
TR(kCircleBack, "CIRCLE: back to menu", "CÍRCULO: volver al menú", "CERCLE: tornar al menú",
   "ROND : retour au menu", "KREIS: zurück zum Menü", "CERCHIO: torna al menu",
   "CÍRCULO: voltar ao menu")
TR(kLoadErrNoLoader, "There is no loader for this file.", "No hay un cargador para este fichero.",
   "No hi ha cap carregador per a aquest fitxer.", "Aucun chargeur pour ce fichier.",
   "Für diese Datei gibt es keinen Lader.", "Nessun caricatore per questo file.",
   "Não há carregador para este arquivo.")
TR(kLoadErrLoader, "The game could not be loaded.", "No se ha podido cargar el juego.",
   "No s'ha pogut carregar el joc.", "Impossible de charger le jeu.",
   "Das Spiel konnte nicht geladen werden.", "Impossibile caricare il gioco.",
   "Não foi possível carregar o jogo.")
TR(kLoadErrEncrypted,
   "The game is encrypted: it must be decrypted before use.",
   "El juego está cifrado: hay que descifrarlo antes de usarlo.",
   "El joc està xifrat: cal desxifrar-lo abans de fer-lo servir.",
   "Le jeu est chiffré : il faut le déchiffrer avant de l'utiliser.",
   "Das Spiel ist verschlüsselt: es muss vorher entschlüsselt werden.",
   "Il gioco è cifrato: va decifrato prima di usarlo.",
   "O jogo está criptografado: é preciso descriptografá-lo antes.")
TR(kLoadErrFormat, "Unsupported file format.", "Formato de fichero no soportado.",
   "Format de fitxer no compatible.", "Format de fichier non pris en charge.",
   "Nicht unterstütztes Dateiformat.", "Formato di file non supportato.",
   "Formato de arquivo não suportado.")
TR(kLoadErrGba,
   "It's a GBA Virtual Console title, not supported.",
   "Es un título de Consola Virtual de GBA, no soportado.",
   "És un títol de Consola Virtual de GBA, no compatible.",
   "C'est un titre Console Virtuelle GBA, non pris en charge.",
   "Das ist ein GBA-Virtual-Console-Titel, nicht unterstützt.",
   "È un titolo Virtual Console GBA, non supportato.",
   "É um título do Virtual Console de GBA, não suportado.")
TR(kLoadErrSystemFiles, "3DS system files are missing.", "Faltan ficheros de sistema del 3DS.",
   "Falten fitxers de sistema de la 3DS.", "Des fichiers système 3DS manquent.",
   "Es fehlen 3DS-Systemdateien.", "Mancano file di sistema del 3DS.",
   "Faltam arquivos de sistema do 3DS.")
TR(kLoadErrNotInitialized, "The CPU core has not been initialized.",
   "El núcleo de CPU no se ha inicializado.", "El nucli de CPU no s'ha inicialitzat.",
   "Le coeur CPU n'a pas été initialisé.", "Der CPU-Kern wurde nicht initialisiert.",
   "Il core della CPU non è stato inizializzato.", "O núcleo da CPU não foi inicializado.")
TR(kLoadErrRead,
   "The ROM could not be read (unreadable header, keys, or corrupt file).",
   "No se ha podido leer el ROM (cabecera ilegible, claves, o fichero corrupto).",
   "No s'ha pogut llegir la ROM (capçalera il·legible, claus, o fitxer corrupte).",
   "Impossible de lire la ROM (en-tête illisible, clés ou fichier corrompu).",
   "ROM nicht lesbar (Header unlesbar, Schlüssel oder beschädigte Datei).",
   "Impossibile leggere la ROM (intestazione illeggibile, chiavi o file corrotto).",
   "Não foi possível ler a ROM (cabeçalho ilegível, chaves ou arquivo corrompido).")
TR(kLoadErrUnknown, "Unknown error while loading the game.",
   "Error desconocido al cargar el juego.", "Error desconegut en carregar el joc.",
   "Erreur inconnue au chargement du jeu.", "Unbekannter Fehler beim Laden des Spiels.",
   "Errore sconosciuto nel caricare il gioco.", "Erro desconhecido ao carregar o jogo.")

// --- Arranque ----------------------------------------------------------------
TR(kStarting, "Starting...", "Iniciando...", "Iniciant...", "Démarrage...", "Wird gestartet...",
   "Avvio...", "Iniciando...")
TR(kStep1, "1/7 creating folders", "1/7 creando carpetas", "1/7 creant carpetes",
   "1/7 création des dossiers", "1/7 Ordner werden erstellt", "1/7 creazione cartelle",
   "1/7 criando pastas")
TR(kStep2, "2/7 starting the event log", "2/7 iniciando registro de eventos",
   "2/7 iniciant el registre d'esdeveniments", "2/7 démarrage du journal",
   "2/7 Ereignisprotokoll wird gestartet", "2/7 avvio registro eventi",
   "2/7 iniciando registro de eventos")
TR(kStep3, "3/7 applying settings", "3/7 aplicando ajustes", "3/7 aplicant ajustos",
   "3/7 application des paramètres", "3/7 Einstellungen werden angewendet",
   "3/7 applicazione impostazioni", "3/7 aplicando configurações")
TR(kStep4, "4/7 setting up controls", "4/7 configurando mandos", "4/7 configurant els controls",
   "4/7 configuration des commandes", "4/7 Steuerung wird eingerichtet",
   "4/7 configurazione comandi", "4/7 configurando controles")
TR(kStep5, "5/7 preparing the core", "5/7 preparando el núcleo", "5/7 preparant el nucli",
   "5/7 préparation du coeur", "5/7 Kern wird vorbereitet", "5/7 preparazione del core",
   "5/7 preparando o núcleo")
TR(kStep6, "6/7 registering applets", "6/7 registrando applets", "6/7 registrant applets",
   "6/7 enregistrement des applets", "6/7 Applets werden registriert",
   "6/7 registrazione applet", "6/7 registrando applets")
TR(kStep7, "7/7 looking for games", "7/7 buscando juegos", "7/7 cercant jocs",
   "7/7 recherche des jeux", "7/7 Spiele werden gesucht", "7/7 ricerca giochi",
   "7/7 procurando jogos")
TR(kMemoryLine, "user memory %d MB total, %d MB free  (extended mode %s)",
   "mem usuario %d MB totales, %d MB libres  (modo ampliado %s)",
   "mem usuari %d MB totals, %d MB lliures  (mode ampliat %s)",
   "mém. utilisateur %d Mo au total, %d Mo libres  (mode étendu %s)",
   "Benutzerspeicher %d MB gesamt, %d MB frei  (erweiterter Modus %s)",
   "mem utente %d MB totali, %d MB liberi  (modalità estesa %s)",
   "mem usuário %d MB no total, %d MB livres  (modo ampliado %s)")
TR(kHeapCdramLine, "heap %u MB    free CDRAM %d MB", "heap %u MB    CDRAM libre %d MB",
   "heap %u MB    CDRAM lliure %d MB", "tas %u Mo    CDRAM libre %d Mo",
   "Heap %u MB    CDRAM frei %d MB", "heap %u MB    CDRAM libera %d MB",
   "heap %u MB    CDRAM livre %d MB")
TR(kYes, "YES", "SÍ", "SÍ", "OUI", "JA", "SÌ", "SIM")
TR(kNo, "no", "no", "no", "non", "nein", "no", "não")
