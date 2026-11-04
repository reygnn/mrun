# mrun – minimaler Ausführen-Dialog für Wayland (passend zu mwc)

Eine C-Datei, nur libwayland, xkbcommon, Cairo und Pango, kein Toolkit. Motif-Look wie
[mfm](https://github.com/reygnn/mfm), mit denselben Farben wie
[mwc](https://github.com/reygnn/mwc): `MWC_INACTIVE` / `MWC_ACTIVE` aus der Umgebung,
sonst `Mwm*background` / `Mwm*activeBackground` aus `~/.config/mwc/mwmrc`.

mrun liest beim Start alle Programme aus `$PATH` und filtert beim Tippen: zuerst der
exakte Name, dann Namen, die so anfangen, dann Namen, die es enthalten, Groß/klein egal.
Nach dem Start eines Programms beendet es sich.

## Bauen

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install        # -> /usr/local/bin/mrun

In mwc startet es die mittlere Maustaste auf dem Desktop.

## Bedienung

| Eingabe | Aktion |
|---|---|
| tippen | Liste filtern |
| ↑ ↓ Bild↑ Bild↓ | Programm wählen |
| Enter | starten |
| Tab | gewählten Namen übernehmen, dahinter ein Leerzeichen für Argumente |
| ← → Pos1 Ende Backspace Entf | Zeile bearbeiten |
| Ctrl+U | Zeile leeren |
| Esc / Ctrl+Q / Ctrl+W | beenden |
| Klick / Doppelklick | wählen / starten |
| Mausrad | Liste scrollen |

**Was Enter startet:**
- **Ohne Leerzeichen** das gewählte Programm, direkt und ohne Shell. So startet `fire`
  schon Firefox, wenn es der erste Treffer ist. Ohne Treffer meldet die Statuszeile das,
  außer die Zeile enthält einen `/`: Dann geht sie an die Shell.
- **Mit Leerzeichen** die Zeile, so wie sie dasteht, mit `/bin/sh -c`. Argumente und
  Shell-Syntax gehen also, etwa `firefox --private-window` oder `sleep 5; foot`.
- **Bei leerer Zeile** nichts, solange nichts gewählt ist.

Programme laufen in einer eigenen Sitzung und ohne Terminal. Reine Terminal-Programme wie
`htop` startet man also so: `foot htop`. Ob ein Programm mit Leerzeichen-Zeile wirklich
gestartet ist, sieht mrun nicht, es ist dann schon beendet.

Einträge in `$PATH`, die nicht absolut sind, zählen nicht. Namen, die kein gültiges UTF-8
sind oder Steuerzeichen enthalten, erscheinen nicht in der Liste; mit Pfad und Leerzeichen
lassen sie sich trotzdem starten.

## Umgebung

| Variable | Standard | |
|---|---|---|
| `MRUN_FONT` | `Sans 10` | Pango-Name, z. B. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | Hintergrund, Auswahl |

## Tests

    make test

fährt mrun unter einem headless laufenden mwc mit simulierter Maus und Tastatur und prüft
mit kleinen Testprogrammen, was gestartet wurde. Es braucht einen gebauten mwc samt
`build/vinput` und `build/pngtool` in `MWC_DIR` (sonst `../mwc` oder `../mwm`) und `grim`.

## Lizenz

MIT, siehe [LICENSE](LICENSE).
