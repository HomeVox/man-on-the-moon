/*
 * Jarvis - bediening vanuit HA: twee helpers die het apparaat elke seconde uitleest
 *   input_number.stopwatch_heftigheid (0..100)  en  input_boolean.stopwatch_microfoon (aan/uit).
 */
#pragma once

namespace jarvis::ctl {

void begin();            // start wifi en de leestaak
float intensity();       // 0..100, standaard 50 tot HA geantwoord heeft
float sensitivity();      // 0..100, standaard 50 (microfoon/drempel)
int mode();                // 0 Audio (golf), 1 Sterrenhemel, 3..6 bollen (bewaard)
void cycleMode();          // knop A: volgende planeet
void audioMode();          // knop B: Audio of Sterrenhemel
bool micOn();            // standaard aan
bool klokOn();           // tijd boven de bollen (standaard aan)
int volume();            // speaker 0..100 (standaard 60)
int helderheid();        // scherm 5..100 (standaard 80)
bool autoSleep();        // na 2 minuten zonder aanraking in deep sleep (standaard aan)
bool nachtklok();        // nachtklok aan (standaard aan)
int nachtVan();          // nachtklok vanaf dit uur (standaard 22)
int nachtTot();          // tot dit uur (standaard 7)
bool nachtNu();          // nachtstand met de hand aan (ook buiten van/tot)
int melkweg();           // 0..100: hoe sterk de melkweg op de muziek reageert (standaard 50)
bool takeSleepNow();     // knop Slapen in HA ingedrukt (één keer waar)
void goingToSleep();     // HA laten weten dat we weg zijn
bool connected();        // laatste lezing gelukt

}  // namespace jarvis::ctl
