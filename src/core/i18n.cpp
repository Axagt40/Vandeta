#include "i18n.h"
#include "core/config.h"
#include <globals.h>
#include <cstring>

// One row per English UI string, with the four shipped translations. An empty
// cell means "use the English text" - the menu renderer then falls back to the
// source string, so a partial pack never shows a blank row.
//
// The table is deliberately about MENU TEXT: titles, rows and the common
// toggles. Dynamic values (an SSID, an address, a file name) are never in it,
// and a row that ends with a separator (": ") is translated as a prefix so
// "InstaBoot: ON" becomes "Arranque rapido: SI" without an entry per value.
//
// The panel font is ASCII, so the Spanish/German/Portuguese rows avoid accents;
// a UTF-8 byte the built-in font does not carry would print as garbage.
struct Tr {
    const char *en;
    const char *es;
    const char *fr;
    const char *de;
    const char *pt;
};

static const Tr TR[] = {
    // ---- root launcher -----------------------------------------------------
    {"WiFi", "WiFi", "WiFi", "WLAN", "WiFi"},
    {"BLE", "BLE", "BLE", "BLE", "BLE"},
    {"RF", "RF", "RF", "RF", "RF"},
    {"NRF24", "NRF24", "NRF24", "NRF24", "NRF24"},
    {"LoRa", "LoRa", "LoRa", "LoRa", "LoRa"},
    {"FM", "FM", "FM", "FM", "FM"},
    {"IR", "IR", "IR", "IR", "IR"},
    {"Ethernet", "Ethernet", "Ethernet", "Ethernet", "Ethernet"},
    {"GPS", "GPS", "GPS", "GPS", "GPS"},
    {"RFID", "RFID", "RFID", "RFID", "RFID"},
    {"Files", "Archivos", "Fichiers", "Dateien", "Arquivos"},
    {"Apps", "Apps", "Applis", "Apps", "Apps"},
    {"Clock", "Reloj", "Horloge", "Uhr", "Relogio"},
    {"Others", "Otros", "Autres", "Mehr", "Outros"},
    {"Config", "Config", "Config", "Einstell.", "Config"},

    // ---- Config page -------------------------------------------------------
    {"Display & UI", "Pantalla e IU", "Affichage", "Anzeige", "Tela e IU"},
    {"LED Config", "Config LED", "Config LED", "LED-Konfig", "Config LED"},
    {"Audio Config", "Config audio", "Config audio", "Audio-Konfig", "Config audio"},
    {"System Config", "Config sistema", "Config systeme", "Systemkonfig", "Config sistema"},
    {"HID Settings", "Ajustes HID", "Reglages HID", "HID-Optionen", "Ajustes HID"},
    {"Battery", "Bateria", "Batterie", "Akku", "Bateria"},
    {"Power", "Energia", "Alimentation", "Strom", "Energia"},
    {"Install App Store", "Instalar tienda", "Installer App Store", "App Store", "Instalar loja"},
    {"About", "Acerca de", "A propos", "Info", "Sobre"},

    // ---- Display & UI ------------------------------------------------------
    {"Brightness", "Brillo", "Luminosite", "Helligkeit", "Brilho"},
    {"Dim Time", "Tiempo de atenuar", "Delai de veille", "Dimm-Zeit", "Tempo de escurecer"},
    {"Orientation", "Orientacion", "Orientation", "Ausrichtung", "Orientacao"},
    {"UI Color", "Color IU", "Couleur IU", "UI-Farbe", "Cor da IU"},
    {"UI Theme", "Tema IU", "Theme IU", "UI-Design", "Tema da IU"},

    // ---- System Config -----------------------------------------------------
    {"InstaBoot: ", "Arranque rapido: ", "Demarrage rapide: ", "Schnellstart: ", "Inicio rapido: "},
    {"WiFi Startup: ", "WiFi al inicio: ", "WiFi au demarrage: ", "WLAN beim Start: ", "WiFi ao iniciar: "},
    {"WiFi Hop Plan: ", "Plan de salto WiFi: ", "Plan de saut WiFi: ", "WLAN-Hopplan: ", "Plano de salto WiFi: "},
    {"Startup App", "App de inicio", "App au demarrage", "Start-App", "App de inicio"},
    {"Hide/Show Apps", "Ocultar/mostrar apps", "Masquer les apps", "Apps ein/aus", "Ocultar/mostrar apps"},
    {"Keyboard Language: ", "Idioma del teclado: ", "Langue du clavier: ", "Tastatursprache: ", "Idioma do teclado: "},
    {"Language: ", "Idioma: ", "Langue: ", "Sprache: ", "Idioma: "},
    {"Language", "Idioma", "Langue", "Sprache", "Idioma"},
    {"Developer Options", "Opciones de desarrollo", "Options developpeur", "Entwickleroptionen", "Opcoes de dev"},
    {"Advanced", "Avanzado", "Avance", "Erweitert", "Avancado"},
    {"Set Device pins", "Pines del dispositivo", "Broches du device", "Geraete-Pins", "Pinos do dispositivo"},
    {"Toggle BLE API", "Activar API BLE", "Activer API BLE", "BLE-API umschalten", "Ativar API BLE"},
    {"BLE name", "Nombre BLE", "Nom BLE", "BLE-Name", "Nome BLE"},
    {"Network Creds", "Credenciales de red", "Identifiants reseau", "Netzwerk-Zugang", "Credenciais de rede"},
    {"Factory Reset", "Restablecer de fabrica", "Reinitialiser", "Werksreset", "Restaurar de fabrica"},

    // ---- Battery / Power ---------------------------------------------------
    {"Battery Saver: ", "Ahorro bateria: ", "Eco baterie: ", "Akku-Sparmodus: ", "Economia bateria: "},
    {"Charge Limit: ", "Limite de carga: ", "Limite de charge: ", "Ladelimit: ", "Limite de carga: "},
    {"Back-hold action", "Accion de mantener BACK", "Action maintien BACK", "BACK-Taste halten", "Acao de manter BACK"},

    // ---- LED ---------------------------------------------------------------
    {"LED Color", "Color LED", "Couleur LED", "LED-Farbe", "Cor do LED"},
    {"LED Effect", "Efecto LED", "Effet LED", "LED-Effekt", "Efeito LED"},
    {"LED Brightness", "Brillo LED", "Luminosite LED", "LED-Helligkeit", "Brilho do LED"},
    {"LED Blink: ", "Parpadeo LED: ", "Clignotement LED: ", "LED-Blinken: ", "Piscar LED: "},

    // ---- WiFi page ---------------------------------------------------------
    {"Connect to Wifi", "Conectar a WiFi", "Connexion WiFi", "WLAN verbinden", "Conectar ao WiFi"},
    {"Start WiFi AP", "Iniciar AP WiFi", "Demarrer AP WiFi", "WLAN-AP starten", "Iniciar AP WiFi"},
    {"Attacks", "Ataques", "Attaques", "Angriffe", "Ataques"},
    {"Scan & Monitor", "Escanear y monitorizar", "Scanner et surveiller", "Scannen & Monitor", "Escanear e monitorar"},
    {"Remote Access", "Acceso remoto", "Acces distant", "Fernzugriff", "Acesso remoto"},
    {"WiFi Attacks", "Ataques WiFi", "Attaques WiFi", "WLAN-Angriffe", "Ataques WiFi"},
    {"Evil Wifi Settings", "Ajustes Evil WiFi", "Reglages Evil WiFi", "Evil-WLAN Optionen", "Ajustes Evil WiFi"},
    {"WiFi Config", "Config WiFi", "Config WiFi", "WLAN-Konfig", "Config WiFi"},

    // ---- Common rows -------------------------------------------------------
    {"Main Menu", "Menu principal", "Menu principal", "Hauptmenu", "Menu principal"},
    {"Back", "Atras", "Retour", "Zuruck", "Voltar"},
    {"Start", "Iniciar", "Demarrer", "Start", "Iniciar"},
    {"Stop", "Detener", "Arreter", "Stop", "Parar"},
    {"Start attack", "Iniciar ataque", "Lancer l'attaque", "Angriff starten", "Iniciar ataque"},
    {"Stop + restore", "Detener y restaurar", "Arreter et restaurer", "Stop + wiederherstellen", "Parar e restaurar"},
    {"Captures", "Capturas", "Captures", "Mitschnitte", "Capturas"},
    {"Done", "Listo", "Termine", "Fertig", "Pronto"},
    {"Re-scan", "Reescanear", "Rescanner", "Neu scannen", "Reescanear"},
    {"Select", "Elegir", "Choisir", "Wahlen", "Escolher"},
    {"Save", "Guardar", "Enregistrer", "Speichern", "Salvar"},
    {"Load", "Cargar", "Charger", "Laden", "Carregar"},
    {"Delete", "Borrar", "Supprimer", "Loschen", "Apagar"},
    {"Yes", "Si", "Oui", "Ja", "Sim"},
    {"No", "No", "Non", "Nein", "Nao"},
    {"Scan", "Escanear", "Scanner", "Scannen", "Escanear"},
    {"Scanning...", "Escaneando...", "Scan en cours...", "Scannen...", "Escaneando..."},
    {"Status", "Estado", "Etat", "Status", "Estado"},
    {"Settings", "Ajustes", "Reglages", "Optionen", "Ajustes"},
    {"Tools", "Herramientas", "Outils", "Werkzeuge", "Ferramentas"},
    {"Options", "Opciones", "Options", "Optionen", "Opcoes"},
    {"Refresh", "Actualizar", "Rafraichir", "Aktualisieren", "Atualizar"},
    {"Reset", "Reiniciar", "Reinitialiser", "Zurucksetzen", "Reiniciar"},
    {"Cancel", "Cancelar", "Annuler", "Abbrechen", "Cancelar"},
    {"Confirm", "Confirmar", "Confirmer", "Bestatigen", "Confirmar"},
    {"Error", "Error", "Erreur", "Fehler", "Erro"},
    {"Warning", "Aviso", "Attention", "Warnung", "Aviso"},
    {"Ready", "Listo", "Pret", "Bereit", "Pronto"},
    {"Running", "En marcha", "En cours", "Lauft", "Em execucao"},
    {"Enabled", "Activado", "Active", "An", "Ativado"},
    {"Disabled", "Desactivado", "Desactive", "Aus", "Desativado"},
    {"Version", "Version", "Version", "Version", "Versao"},
    {"Device", "Dispositivo", "Appareil", "Geraet", "Dispositivo"},
    {"Time", "Hora", "Heure", "Zeit", "Hora"},
    {"Date", "Fecha", "Date", "Datum", "Data"},

    // ---- MITM page ---------------------------------------------------------
    {"Mode: ", "Modo: ", "Mode: ", "Modus: ", "Modo: "},
    {"Target: ", "Objetivo: ", "Cible: ", "Ziel: ", "Alvo: "},
    {"Payload: ", "Payload: ", "Payload: ", "Payload: ", "Payload: "},
    {"Network: ", "Red: ", "Reseau: ", "Netzwerk: ", "Rede: "},
    {"Save logs: ", "Guardar logs: ", "Sauver les logs: ", "Logs speichern: ", "Salvar logs: "},
    {"Preview payload", "Ver payload", "Voir le payload", "Payload ansehen", "Ver payload"},
    {"MITM Attack", "Ataque MITM", "Attaque MITM", "MITM-Angriff", "Ataque MITM"},
    {"MITM Targets", "Objetivos MITM", "Cibles MITM", "MITM-Ziele", "Alvos MITM"},
    {"MITM Network", "Red MITM", "Reseau MITM", "MITM-Netz", "Rede MITM"},
    {"MITM RUN", "MITM ACTIVO", "MITM ACTIF", "MITM LAUFT", "MITM ATIVO"},
    {"MITM Captures", "Capturas MITM", "Captures MITM", "MITM-Mitschnitte", "Capturas MITM"},
    {"All devices", "Todos los dispositivos", "Tous les appareils", "Alle Geraete", "Todos os dispositivos"},
    {"Single (none picked)", "Uno (sin elegir)", "Un seul (aucun)", "Eins (keins)", "Um (nenhum)"},
    {"Multi (none picked)", "Varios (sin elegir)", "Plusieurs (aucun)", "Mehrere (keins)", "Varios (nenhum)"},

    // ---- RF pages ----------------------------------------------------------
    {"Wardriver", "Wardriver", "Wardriver", "Wardriver", "Wardriver"},
    {"Wardriver Config", "Config Wardriver", "Config Wardriver", "Wardriver-Konfig", "Config Wardriver"},
    {"Jammer", "Jammer", "Brouilleur", "Storer", "Jammer"},
    {"Spectrum", "Espectro", "Spectre", "Spektrum", "Espectro"},
    {"Replay", "Reproducir", "Rejouer", "Abspielen", "Reproduzir"},
    {"Record", "Grabar", "Enregistrer", "Aufnehmen", "Gravar"},
    {"Browse", "Explorar", "Parcourir", "Durchsuchen", "Explorar"},
};

static const size_t TR_COUNT = sizeof(TR) / sizeof(TR[0]);

int languageCount() { return (int)LANG_COUNT; }

const char *languageName(int lang) {
    switch (lang) {
        case LANG_ES: return "Espanol";
        case LANG_FR: return "Francais";
        case LANG_DE: return "Deutsch";
        case LANG_PT: return "Portugues";
        default: return "English";
    }
}

static const char *trColumn(const Tr &row, int lang) {
    const char *v = nullptr;
    switch (lang) {
        case LANG_ES: v = row.es; break;
        case LANG_FR: v = row.fr; break;
        case LANG_DE: v = row.de; break;
        case LANG_PT: v = row.pt; break;
        default: return row.en;
    }
    return (v && *v) ? v : row.en;
}

/// "ON"/"OFF" are the values every toggle row ends with, and they are values,
/// not labels - so they are translated here rather than carried by every row.
static const char *trOnOff(const String &value, int lang) {
    bool on = value == "ON" || value == "on" || value == "On";
    bool off = value == "OFF" || value == "off" || value == "Off";
    if (!on && !off) return nullptr;
    switch (lang) {
        case LANG_ES: return on ? "SI" : "NO";
        case LANG_FR: return on ? "OUI" : "NON";
        case LANG_DE: return on ? "EIN" : "AUS";
        case LANG_PT: return on ? "SIM" : "NAO";
        default: return nullptr;
    }
}

String i18n(const char *text) {
    if (text == nullptr) return String();
    return i18n(String(text));
}

String i18n(const String &text) {
    if (text.length() == 0) return text;
    const int lang = bruceConfig.language;
    if (lang <= LANG_EN || lang >= LANG_COUNT) return text;

    if (const char *w = trOnOff(text, lang)) return String(w);

    for (size_t i = 0; i < TR_COUNT; i++) {
        if (text == TR[i].en) return String(trColumn(TR[i], lang));
    }

    // A row such as "InstaBoot: ON" is stored once as a prefix. Take the
    // LONGEST matching prefix so "LED Blink: " wins over "LED ".
    const Tr *best = nullptr;
    size_t bestLen = 0;
    for (size_t i = 0; i < TR_COUNT; i++) {
        size_t n = strlen(TR[i].en);
        if (n == 0 || n >= text.length()) continue;
        if (n <= bestLen) continue;
        if (strncmp(text.c_str(), TR[i].en, n) == 0) {
            best = &TR[i];
            bestLen = n;
        }
    }
    if (best == nullptr) return text;

    String rest = text.substring(bestLen);
    if (const char *w = trOnOff(rest, lang)) return String(trColumn(*best, lang)) + String(w);
    return String(trColumn(*best, lang)) + rest;
}
