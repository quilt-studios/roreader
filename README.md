<p align="center">
  <img src="assets/roreader-logo.png" alt="ROREADER" width="420">
</p>

# ROREADER

**Ein EPUB-Reader mit Terminal, Sketchpad und Texteditor für Kobo und kompatible tolino-Geräte.**

ROREADER baut auf Kobalt auf und verwendet dessen Qt-Oberfläche und CREngine-Renderer. Du kannst DRM-freie EPUBs lesen, Zeichnungen anfertigen, Textdateien bearbeiten und Shell-Kommandos direkt auf dem Reader ausführen. Geschützte Bücher, PDFs und andere nicht unterstützte Formate werden weiterhin vom eingebauten Reader geöffnet.

## Werkzeuge auf dem Gerät

Öffne in einem Buch **Einstellungen → System → Terminal · Sketchpad · Texteditor**.

| Werkzeug | Funktionen |
| --- | --- |
| Terminal | Shell-Kommandos mit sichtbarer Ausgabe, Bildschirmtastatur und Stopptaste. Jeder Befehl läuft separat, maximal 60 Sekunden und mit begrenzter Ausgabe. Interaktive Vollbildprogramme wie `vi` werden nicht unterstützt. |
| Sketchpad | Mit Finger oder Maus zeichnen, radieren, rückgängig machen und als PNG speichern. |
| Texteditor | UTF-8-Dateien öffnen und atomar speichern, Text per Bildschirmtastatur einfügen, neue Zeilen und Rückgängig. Dateien bis 1 MiB. Extern geänderte oder ungeöffnet vorhandene Dateien werden nicht still überschrieben. |

Texte und Zeichnungen liegen auf dem Reader in **`/mnt/onboard/ROREADER/`**, also im Ordner **`ROREADER`** auf dem USB-Laufwerk. Auf dem Linux-Desktop liegen sie unter `Dokumente/ROREADER`. Ungespeicherte Entwürfe bleiben beim Wechsel zwischen den Werkzeugen erhalten; beim Schließen fragt die Oberfläche nach. Zum dauerhaften Aufbewahren bitte speichern. Shell-Kommandos laufen mit den Rechten des Reader-Prozesses; prüfe Befehle vor dem Ausführen.

## Windows: in Chrome installieren

1. Lade [ROREADER-Windows-Installer.zip](ziprelease/ROREADER-Windows-Installer.zip) herunter und entpacke es vollständig auf deinem Windows-PC. Die [SHA-256-Prüfsumme](ziprelease/ROREADER-Windows-Installer.zip.sha256) liegt daneben.
2. Verbinde den Reader per USB und bestätige am Gerät die Verbindung zum Computer. Sichere deine Bücher und Lesedaten.
3. Starte **`Install-ROREADER.bat`**. Die Datei öffnet den lokalen Installer in **Google Chrome**. Chrome muss installiert sein; Python und Administratorrechte werden nicht benötigt.
4. Klicke **„Reader-Laufwerk auswählen“** und wähle das **Hauptverzeichnis `/`** deines Geräts, zum Beispiel `E:\`. Wähle weder `.kobo` noch `.adds` als Ausgangsordner.
5. Erlaube Chrome den Zugriff auf diesen Ordner. Das mitgelieferte Qt6-Paket und seine SHA-256-Prüfsumme werden automatisch geladen. Prüfe die erkannte Firmware und bestätige deine Datensicherung.
6. Klicke **„ROREADER auf den Reader kopieren“**. Nach erfolgreicher Kopierprüfung das Laufwerk sicher auswerfen, USB trennen und die Installation auf dem Reader abwarten.

Lass das Konsolenfenster währenddessen geöffnet. Es betreibt ausschließlich den lokalen Dateiserver; das Schließen beendet ihn. Dateien werden direkt über Chromes Ordnerfreigabe auf das Gerät geschrieben. Es gibt keinen Upload und keine Anmeldung. Ein bereits auf dem Gerät liegendes Update-Archiv wird nicht überschrieben.

Wenn du die BAT direkt aus dem Quellcode startest, kannst du stattdessen dein gebautes **`ROREADER-qt6.zip`** und die Prüfsumme aus **`ROREADER-qt6.zip.sha256`** auswählen. Die Weboberfläche benötigt ein echtes Gerätepaket; sie lädt keine fremde Release-Version als Ersatz.

## Firmware und Modelle

Der Installationsweg richtet sich an **Kobo- und tolino-Geräte mit Kobo-basierter Firmware 5.x oder 6.x** und verwendet das Qt6-Paket. Der Installer prüft die Firmwarefamilie anhand von `.kobo/version`; die Anwendung behält ihre Prüfungen der benötigten Firmware-Symbole und Objektlayouts.

**Eine Unterstützung aller Modelle und aller Firmwarestände ist noch nicht auf echten Geräten nachgewiesen.** Die erkannte Versionsnummer bestätigt weder ein kompatibles Gerät noch eine erfolgreich abgeschlossene Installation. Ältere Android-basierte tolino-Modelle sind nicht Teil dieses USB-Installationswegs. Ein gegebenenfalls benötigter Mod-Manager muss ebenfalls zur installierten Firmware passen. Veränderte Firmware-ABIs dürfen die Kompatibilitätsprüfungen nicht umgehen.

Die vorhandenen Qt5- und Android-Buildpfade bleiben im Quellcode. Der neue Windows-Installer akzeptiert ausschließlich das normale Qt6-Gerätepaket, keine Qt5- oder Evaluationspakete.

## Selbst bauen und testen

Ausführliche Voraussetzungen und Plattformbefehle stehen in [Building](doc/building.md). Linux-Entwicklung verwendet ein vorbereitetes **Qt 6.11+ SDK** mit Core, Gui, Widgets, Network und Sql; Tests benötigen zusätzlich Test, passende GuiPrivate-Header und das Minimal-Plugin. Qt wird nicht als Teil von ROREADER kompiliert.

```sh
cd platforms/desktop
cmake --preset linux -DCMAKE_PREFIX_PATH=/pfad/zum/qt-sdk
cmake --build --preset linux
cmake --build --preset linux-light-tests
ctest --preset linux-light
```

Der interne Desktop-Buildtarget und die ausführbare Datei heißen aus Kompatibilitätsgründen weiterhin `Kobalt`; Oberfläche und Gerätepakete heißen ROREADER. Auch `.adds/kobalt`, bestehende Profile, Datenbanken und Plugin-Identitäten bleiben erhalten, damit vorhandene Lesedaten weiterverwendet werden. Quellcode-Namensräume und CMake-Optionen mit `KOBALT_` sind keine separaten Anwendungen.

Für das Firmware-5/6-Paket wird das gepinnte, vorbereitete `kobuild`-Qt6-Image benötigt:

```sh
docker pull "$(sh platforms/sdk-image.sh qt6)"
bash platforms/kobo/build.sh qt6
cmake -DROOT="$PWD" \
  -DPACKAGE_FILE="$PWD/build/kobo-qt6/ROREADER-qt6.zip" \
  -P platforms/installer/package.cmake
```

Das Gerätepaket liegt in `build/kobo-qt6/`; das Windows-Bundle inklusive BAT, Weboberfläche, Paket und Prüfsumme liegt in **`build/installer/ROREADER-Windows-Installer.zip`**. Build und Paketprüfung ersetzen keinen Installationstest auf dem Gerät.

Die ZIP- und Firmwareprüfungen der Weboberfläche lassen sich mit Node.js ausführen:

```sh
node --test tests/installer.test.mjs
```

Die native Light-Suite enthält auch die Funktionsprüfungen der Gerätewerkzeuge. Screenshots laufen headless über `linux-light-screenshots`; die vollständige Galerie und weitere Testgruppen sind in [Native validation](doc/validation.md) beschrieben. Der Entwickler-Launcher startet über `./start.sh`; `./start.sh --check` prüft seine Voraussetzungen, ohne ein Fenster zu öffnen.

## Herkunft und Lizenz

ROREADER ist ein Fork von Kobalt, kein offizielles Produkt von Rakuten Kobo oder tolino. Die ursprünglichen Urheberhinweise und [Acknowledgments](doc/acknowledgements.md) bleiben erhalten. CREngine stammt aus dem [KOReader](https://koreader.rocks/)-Umfeld. Die neuen Logo-Grafiken wurden für ROREADER generiert.

Der Anwendungscode steht unter der [MIT-Lizenz](LICENSE); Drittbibliotheken und kombinierte Pakete unterliegen ihren jeweiligen Lizenzbedingungen, einschließlich GPL und AGPL. Das kombinierte Paket wird unter GNU AGPL v3 ohne Gewährleistung verteilt. Siehe `assets/notices/` für die enthaltenen Lizenztexte.
