// =====================================================================================
// NeoPixel 8×8 — rotation/mirrors matériels + modes, HS EEPROM, RNG fort, scroll texte
// Libre d'utilisation — 2025
//
// JEU ARCADE :
// - Le joueur est une barre de 2 pixels en bas
// - Des ennemis tombent depuis le haut
// - Le score augmente quand un ennemi sort par le bas
// - Bonus bleu = +1 vie
// - High Score stocké en EEPROM avec vérification d’intégrité
// =====================================================================================

#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// =====================================================================================
// Compatibilité PROGMEM (AVR / autres architectures)
// =====================================================================================
#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
#else
  #ifndef PROGMEM
    #define PROGMEM
  #endif
  #ifndef pgm_read_byte
    #define pgm_read_byte(addr) (*(const unsigned char *)(addr))
  #endif
#endif

// =====================================================================================
// Anti-conflits de macros WIDTH / HEIGHT (souvent définies ailleurs)
// =====================================================================================
#ifdef WIDTH
  #undef WIDTH
#endif
#ifdef HEIGHT
  #undef HEIGHT
#endif

// =====================================================================================
// DEBUG série (optionnel)
// =====================================================================================
// 0 = désactivé
// 1 = affiche score et vies sans spam
#define SERIAL_DEBUG_SCORE_LIVES 0

// =====================================================================================
// Configuration matrice
// =====================================================================================
#define WIDTH  8
#define HEIGHT 8

// 0 = câblage linéaire
// 1 = câblage serpentin (zigzag)
#define WIRING_ZIGZAG 1

// =====================================================================================
// Rotation / miroir matériel
// Permet d’adapter le code à n’importe quel sens de montage physique
// =====================================================================================
#define MATRIX_ROTATION 180   // 0 / 90 / 180 / 270
#define MATRIX_MIRROR_X 0
#define MATRIX_MIRROR_Y 0

// =====================================================================================
// Gameplay
// =====================================================================================
// 0 = bords bloquants
// 1 = wrap horizontal (le joueur traverse)
#define PLAYER_WRAP_EDGES 1

// Points bonus lors d’un wrap complet
#define WRAP_WRAP_SCORE_BONUS 10

// =====================================================================================
// Texte
// =====================================================================================
#define SCORE_SCROLL_SPEED_MS 90
#define TEXT_ROT_180 1   // rotation du texte indépendamment du mapping LED

// =====================================================================================
// Vies & bonus
// =====================================================================================
#define MAX_LIVES 2
#define BONUS_EVERY_SCORE 15

// =====================================================================================
// Son
// =====================================================================================
#define SOUND_ENABLED 1

// =====================================================================================
// Brochage matériel
// =====================================================================================
const uint8_t MATRIX_PIN    = 6;
const uint16_t LED_COUNT    = WIDTH * HEIGHT;
const uint8_t LEFT_BTN_PIN  = 5;
const uint8_t RIGHT_BTN_PIN = 7;
const uint8_t BUZZER_PIN    = 9;

Adafruit_NeoPixel pixels(LED_COUNT, MATRIX_PIN, NEO_GRB + NEO_KHZ800);

// =====================================================================================
// Sons (désactivables globalement)
// =====================================================================================
void playStartSound(){
#if SOUND_ENABLED
  tone(BUZZER_PIN,523,100); delay(130);
  tone(BUZZER_PIN,659,100); delay(130);
  tone(BUZZER_PIN,784,120); delay(150);
  noTone(BUZZER_PIN);
#endif
}

void playLoseSound(){
#if SOUND_ENABLED
  tone(BUZZER_PIN,784,120); delay(150);
  tone(BUZZER_PIN,659,120); delay(150);
  tone(BUZZER_PIN,523,180); delay(210);
  noTone(BUZZER_PIN);
#endif
}

void playNewRecordSound(){
#if SOUND_ENABLED
  tone(BUZZER_PIN,523,90);  delay(115);
  tone(BUZZER_PIN,659,90);  delay(115);
  tone(BUZZER_PIN,784,90);  delay(115);
  tone(BUZZER_PIN,988,160); delay(190);
  noTone(BUZZER_PIN);
#endif
}

void playLifeSound(){
#if SOUND_ENABLED
  tone(BUZZER_PIN,988,120); delay(140);
  noTone(BUZZER_PIN);
#endif
}

// =====================================================================================
// Mode jour / nuit (luminosité)
// =====================================================================================
#define BRIGHTNESS_DAY   120
#define BRIGHTNESS_NIGHT 15
bool isDayMode = false;

// =====================================================================================
// Couleurs
// =====================================================================================
const uint8_t COLOR_INTENSITY = 50;
uint32_t COLOR_BG, COLOR_PLAYER, COLOR_ENEMY, COLOR_TEXT, COLOR_RECORD;
uint32_t COLOR_BONUS;
uint32_t COLOR_PLAYER_1LIFE;
uint32_t COLOR_PLAYER_2LIVES;
uint32_t COLOR_PLAYER_3LIVES;

// =====================================================================================
// Joueur
// =====================================================================================
int playerY = HEIGHT - 1;
int playerLeftX  = 3;
int playerRightX = 4;
uint8_t playerLives = 0;

// =====================================================================================
// Ennemis
// =====================================================================================
const uint8_t MAX_ENEMIES = 4;
bool enemyActive[MAX_ENEMIES];
bool enemyJustSpawned[MAX_ENEMIES];
int  enemyX[MAX_ENEMIES];
int  enemyY[MAX_ENEMIES];

// =====================================================================================
// Bonus (un seul à la fois)
// =====================================================================================
bool bonusActive = false;
int  bonusX = 0;
int  bonusY = 0;
long lastBonusScore = -BONUS_EVERY_SCORE;

// =====================================================================================
// Timings dynamiques (difficulté progressive)
// =====================================================================================
const unsigned long START_MOVE_MS   = 230;  // plus grand, moins vite au début [default 220]
const unsigned long MIN_MOVE_MS     = 60;   // plus petit, vitesse minimale plus rapide default [60]
const unsigned long MOVE_ACCEL_MS   = 1;    // plus grand, augmentation de vitesse plus rapide [default  4]

const unsigned long START_SPAWN_MS  = 1100; // plus grand, moins de spawn au début [default 1100]
const unsigned long MIN_SPAWN_MS    = 250;  // [default 250]
const unsigned long SPAWN_ACCEL_MS  = 60;   // plus grand, plus de spawn [defaul 15]

unsigned long enemyMoveInterval;
unsigned long enemySpawnInterval;

unsigned long lastMoveAt   = 0;
unsigned long lastSpawnAt  = 0;
unsigned long lastInputAt  = 0;
unsigned long inputRepeatMs = 95;

// =====================================================================================
// Score / High Score
// =====================================================================================
const uint16_t SCORE_PER_LED = 8;
long score     = 0;
long highScore = 0;
bool lost      = false;
bool needShow  = false;
bool newRecordJustSet = false;

// =====================================================================================
// RNG renforcé (évite patterns répétitifs)
// =====================================================================================
static uint32_t entropy_pool = 0x12345678UL;
static bool reseededFromFirstInput = false;

static inline uint32_t rotl32(uint32_t x, uint8_t r){
  return (x<<r)|(x>>(32-r));
}

static inline void rng_mix(uint32_t v){
  entropy_pool ^= v + 0x9E3779B9UL;
  entropy_pool = rotl32(entropy_pool, (uint8_t)(micros() & 31));
  entropy_pool ^= (uint32_t)millis();
}


// =====================================================================================
// --- FIX: #if multi-lignes, pas de code sur la même ligne que #if / #endif
// Objectif : ajouter de l’entropie (bruit) via des lectures analogiques si dispo.
// =====================================================================================
static inline void rng_addAnalogOnce(){
  #if defined(A0)
    rng_mix(((uint32_t)analogRead(A0)) << 16);
  #endif
  #if defined(A1)
    rng_mix(((uint32_t)analogRead(A1)) << 8);
  #endif
  #if defined(A2)
    rng_mix((uint32_t)analogRead(A2));
  #endif
}

// Récupère un peu d’entropie au démarrage en bouclant quelques ms
static void rng_gatherStartupEntropy(uint16_t ms=25){
  unsigned long endT=millis()+ms;
  while((long)(endT-millis())>0){
    rng_addAnalogOnce();
    rng_mix((uint32_t)micros());
    delayMicroseconds((uint8_t)(micros()&0x3F));
  }
}

// Applique le "pool" interne comme graine du PRNG Arduino + jette quelques valeurs
static void rng_seedFromPool(){
  randomSeed(entropy_pool);
  for(uint8_t i=0;i<7;i++) (void)random();
}

// Premier seed : combine micros/millis + analog + petites boucles
static void rng_seedInitial(){
  uint32_t s=micros()^millis();
  rng_mix(s);
  rng_addAnalogOnce();
  rng_gatherStartupEntropy(25);
  rng_seedFromPool();
}

// =====================================================================================
// EEPROM High Score
// Stratégie :
// - signature (octet) pour détecter "EEPROM jamais init"
// - valeur 16 bits + son inverse bitwise (anti-corruption simple)
// =====================================================================================
const int EE_ADDR_SIG=0, EE_ADDR_HS_L=1, EE_ADDR_HS_H=2, EE_ADDR_HS_INV_L=3, EE_ADDR_HS_INV_H=4;
const uint8_t HS_SIGNATURE=0xB6;

void saveHighScore(long hs){
  if(hs<0)hs=0;
  if(hs>65535)hs=65535;
  uint16_t v=(uint16_t)hs, inv=~v;

  EEPROM.update(EE_ADDR_SIG,HS_SIGNATURE);
  EEPROM.update(EE_ADDR_HS_L,(uint8_t)(v&0xFF));
  EEPROM.update(EE_ADDR_HS_H,(uint8_t)(v>>8));
  EEPROM.update(EE_ADDR_HS_INV_L,(uint8_t)(inv&0xFF));
  EEPROM.update(EE_ADDR_HS_INV_H,(uint8_t)(inv>>8));
}

long loadHighScore(){
  if(EEPROM.read(EE_ADDR_SIG)!=HS_SIGNATURE) return 0;

  uint16_t v=EEPROM.read(EE_ADDR_HS_L)|((uint16_t)EEPROM.read(EE_ADDR_HS_H)<<8);
  uint16_t inv=EEPROM.read(EE_ADDR_HS_INV_L)|((uint16_t)EEPROM.read(EE_ADDR_HS_INV_H)<<8);

  if((uint16_t)~v!=inv) return 0;
  return (long)v;
}

void updateHighScoreIfNeeded(){
  if(score>highScore){
    highScore=score;
    saveHighScore(highScore);
    newRecordJustSet=true;
  }
}

// =====================================================================================
// Boutons (INPUT_PULLUP → appui = LOW)
// =====================================================================================
inline bool btnLeftPressed(){  return digitalRead(LEFT_BTN_PIN)==LOW; }
inline bool btnRightPressed(){ return digitalRead(RIGHT_BTN_PIN)==LOW; }

// =====================================================================================
// Rotation/Miroir matériel
// Convertit (x,y) logiques → (xo,yo) dans le repère "top-left" du mapping.
// But : tu peux écrire ton jeu en coords logiques standard, et corriger le montage ici.
// =====================================================================================
static inline void mapXYHardware(int x,int y,int &xo,int &yo){
  // 1) rotation
  #if   MATRIX_ROTATION == 0
    xo = x;                 yo = y;
  #elif MATRIX_ROTATION == 90   // horaire
    xo = HEIGHT - 1 - y;    yo = x;
  #elif MATRIX_ROTATION == 180
    xo = WIDTH  - 1 - x;    yo = HEIGHT - 1 - y;
  #elif MATRIX_ROTATION == 270  // anti-horaire
    xo = y;                 yo = WIDTH - 1 - x;
  #else
    #error "MATRIX_ROTATION must be 0/90/180/270"
  #endif

  // 2) miroirs optionnels
  #if MATRIX_MIRROR_X
    xo = WIDTH - 1 - xo;
  #endif
  #if MATRIX_MIRROR_Y
    yo = HEIGHT - 1 - yo;
  #endif
}

// Mapping XY -> index (attend des coords en repère top-left)
// Zigzag : une ligne sur deux est inversée (typique des matrices NeoPixel)
int xyToIndex_topLeft(int x,int y){
  if(x<0||x>=WIDTH||y<0||y>=HEIGHT) return -1;
#if WIRING_ZIGZAG
  if(y%2==0) return y*WIDTH + x;
  else       return y*WIDTH + (WIDTH-1-x);
#else
  return y*WIDTH + x;
#endif
}

// Pose un pixel en coordonnées logiques, en appliquant rotation/miroir + sécurité bornes
void setPixelSafeXY(int x,int y,uint32_t c){
  int xo,yo; mapXYHardware(x,y,xo,yo);
  int idx = xyToIndex_topLeft(xo,yo);
  if(idx>=0){ pixels.setPixelColor(idx,c); needShow=true; }
}

void clearMatrix(){
  for(int i=0;i<(int)LED_COUNT;i++) pixels.setPixelColor(i,0);
  needShow=true;
}

void fillMatrix(uint32_t c){
  for(int i=0;i<(int)LED_COUNT;i++) pixels.setPixelColor(i,c);
  needShow=true;
}

// =====================================================================================
// Gameplay — affichage joueur
// =====================================================================================
void drawPlayer(){
  setPixelSafeXY(playerLeftX,playerY,COLOR_PLAYER);
  setPixelSafeXY(playerRightX,playerY,COLOR_PLAYER);
}

// --- Mode wrap conditionnel (autorise ou bloque le passage par les bords)
bool canMoveLeft(){
  #if PLAYER_WRAP_EDGES
    return true;
  #else
    return playerLeftX > 0;
  #endif
}
bool canMoveRight(){
  #if PLAYER_WRAP_EDGES
    return true;
  #else
    return playerRightX < (WIDTH - 1);
  #endif
}

// Déplacement gauche : efface ancienne position → calcule nouvelle → redraw
void movePlayerLeft(){
  if(!canMoveLeft()) return;

  // Effacer l’ancienne barre
  setPixelSafeXY(playerLeftX,  playerY, COLOR_BG);
  setPixelSafeXY(playerRightX, playerY, COLOR_BG);

  #if PLAYER_WRAP_EDGES
    if (playerLeftX == 0){
      // wrap vers la droite (barre réapparaît à droite)
      playerLeftX  = WIDTH - 2;
      playerRightX = WIDTH - 1;
      score+=WRAP_WRAP_SCORE_BONUS;   // bonus de score si wrap
      updateDifficulty();
    } else {
      playerLeftX--; playerRightX--;
    }
  #else
    playerLeftX--; playerRightX--;
  #endif

  drawPlayer();
}

void movePlayerRight(){
  if(!canMoveRight()) return;

  setPixelSafeXY(playerLeftX,  playerY, COLOR_BG);
  setPixelSafeXY(playerRightX, playerY, COLOR_BG);

  #if PLAYER_WRAP_EDGES
    if (playerRightX == (WIDTH - 1)){
      // wrap vers la gauche
      playerLeftX  = 0;
      playerRightX = 1;
      score+=WRAP_WRAP_SCORE_BONUS;
      updateDifficulty();
    } else {
      playerLeftX++; playerRightX++;
    }
  #else
    playerLeftX++; playerRightX++;
  #endif

  drawPlayer();
}

// =====================================================================================
// Ennemis — génération / déplacement
// =====================================================================================

// Empêche 2 ennemis d’être dans la même colonne (lisibilité + difficulté contrôlée)
bool isColumnTaken(int col){
  for(uint8_t i=0;i<MAX_ENEMIES;i++)
    if(enemyActive[i]&&enemyX[i]==col) return true;
  return false;
}

void trySpawnEnemy(){
  // "micro-jitter" : mélange de temps + random pour casser les répétitions
  rng_mix((uint32_t)micros() ^ (uint32_t)random());

  // Compte ennemis actifs
  uint8_t active=0;
  for(uint8_t i=0;i<MAX_ENEMIES;i++) if(enemyActive[i]) active++;
  if(active>=MAX_ENEMIES) return;

  // Tente de trouver une colonne libre
  for(uint8_t attempt=0;attempt<WIDTH;attempt++){
    int col=(int)random(0,WIDTH);
    if(isColumnTaken(col)) continue;

    // Alloue un slot libre
    for(uint8_t i=0;i<MAX_ENEMIES;i++) if(!enemyActive[i]){
      enemyActive[i]=true;
      enemyX[i]=col;
      enemyY[i]=0;
      enemyJustSpawned[i]=true; // “bloque” le 1er déplacement pour être visible en haut
      setPixelSafeXY(enemyX[i],enemyY[i],COLOR_ENEMY);
      return;
    }
  }
}

void stepEnemies(){
  for(uint8_t i=0;i<MAX_ENEMIES;i++){
    if(!enemyActive[i]) continue;

    // Effacer l’ancienne position si ce n’est pas sur la barre du joueur
    if(enemyY[i] != playerY ||
      (enemyX[i] != playerLeftX && enemyX[i] != playerRightX)){
      setPixelSafeXY(enemyX[i], enemyY[i], COLOR_BG);
    }

    // L’ennemi reste 1 tick en haut après le spawn
    if(enemyJustSpawned[i]){
      enemyJustSpawned[i]=false;
      setPixelSafeXY(enemyX[i],enemyY[i],COLOR_ENEMY);
      continue;
    }

    // Descend d’une ligne
    enemyY[i]++;

    // Si l’ennemi arrive sur un bonus, il le détruit ---
    if(bonusActive &&
       enemyX[i] == bonusX &&
       enemyY[i] == bonusY){
      bonusActive = false;
      setPixelSafeXY(bonusX, bonusY, COLOR_BG);
    }

    // Sortie bas : ennemi supprimé, score +1, difficulté mise à jour
    if(enemyY[i]>=HEIGHT){
      enemyActive[i]=false;
      score++;
      updateDifficulty();
      continue;
    }

    // Redessiner l’ennemi à sa nouvelle position
    setPixelSafeXY(enemyX[i],enemyY[i],COLOR_ENEMY);
  }
}


// =====================================================================================
// Bonus — apparaît tous les BONUS_EVERY_SCORE points, tombe comme un ennemi
// =====================================================================================
void trySpawnBonusByScore(){
  if(bonusActive) return;

  // Déclenchement : score multiple de BONUS_EVERY_SCORE, mais une seule fois par palier
  if(score>0 && (score % BONUS_EVERY_SCORE)==0 && score!=lastBonusScore){
    lastBonusScore = score;

    for(uint8_t attempt=0; attempt<WIDTH; attempt++){
      int col = (int)random(0, WIDTH);
      if(isColumnTaken(col)) continue;

      bonusActive = true;
      bonusX = col;
      bonusY = 0;
      setPixelSafeXY(bonusX, bonusY, COLOR_BONUS);
      return;
    }
  }
}

void stepBonus(){
  if(!bonusActive) return;

  // efface ancienne position
  setPixelSafeXY(bonusX, bonusY, COLOR_BG);

  bonusY++;

  if(bonusY>=HEIGHT){
    bonusActive=false;
    return;
  }

  setPixelSafeXY(bonusX, bonusY, COLOR_BONUS);
}

// =====================================================================================
// Collisions :
// - Bonus : donne une vie (jusqu’à MAX_LIVES), ne tue pas
// - Ennemi : consomme une vie si dispo, sinon défaite
// =====================================================================================
void checkCollision(){
  // BONUS: ne tue pas, donne une vie (max MAX_LIVES)
  if(bonusActive &&
     bonusY == playerY &&
    (bonusX == playerLeftX || bonusX == playerRightX)){

    bonusActive = false;
    setPixelSafeXY(bonusX, bonusY, COLOR_BG);

    score += 10;
    updateDifficulty();   // important pour garder la difficulté cohérente

    if(playerLives < MAX_LIVES){
      playerLives++;
      updatePlayerColor();
      drawPlayer();
      playLifeSound();
    }

    return;   // ESSENTIEL : ne pas enchaîner avec la collision ennemis le même tick
  }

  // ENNEMIS: si vie dispo => consomme vie, sinon mort
  for(uint8_t i=0;i<MAX_ENEMIES;i++) if(enemyActive[i]){
    if((enemyY[i]==playerY)&&(enemyX[i]==playerLeftX || enemyX[i]==playerRightX)){
      if(playerLives>0){
        playerLives--;
        updatePlayerColor();
        drawPlayer();   // redraw immédiat
        setPixelSafeXY(enemyX[i], enemyY[i], COLOR_BG);
        enemyActive[i]=false;
        return;
      } else {
        lost=true;
        bonusActive=false;
        return;
      }
    }
  }
}

// =====================================================================================
// Animation “X” rouge en cas de mort
// =====================================================================================
void flashBigX(uint8_t times=3){
  for(uint8_t t=0;t<times;t++){
    clearMatrix(); pixels.show(); delay(250);

    for(int i=0;i<WIDTH;i++){
      setPixelSafeXY(i,i,COLOR_ENEMY);
      setPixelSafeXY(WIDTH-1-i,i,COLOR_ENEMY);
    }

    pixels.show(); delay(250);
  }
  clearMatrix(); pixels.show();
}

// =====================================================================================
// Police 5×7 (PROGMEM)
// Format : colonnes gauche→droite, bit0=haut .. bit6=bas
// =====================================================================================
const uint8_t GLYPH_SPACE[] PROGMEM = { 0x00 };
const uint8_t GLYPH_COLON[] PROGMEM = { 0x00,0x14,0x00 };
const uint8_t GLYPH_S[]     PROGMEM = { 0x30,0x49,0x49,0x49,0x06 };
const uint8_t GLYPH_C[]     PROGMEM = { 0x3E,0x41,0x41,0x41,0x22 };
const uint8_t GLYPH_O[]     PROGMEM = { 0x3E,0x41,0x41,0x41,0x3E };
const uint8_t GLYPH_R[]     PROGMEM = { 0x7F,0x49,0x49,0x49,0x36 };
const uint8_t GLYPH_E[]     PROGMEM = { 0x7F,0x49,0x49,0x49,0x41 };
const uint8_t GLYPH_H[]     PROGMEM = { 0x7F,0x08,0x08,0x08,0x7F };
const uint8_t GLYPH_0[]     PROGMEM = { 0x3E,0x45,0x49,0x51,0x3E };
const uint8_t GLYPH_1[]     PROGMEM = { 0x00,0x21,0x7F,0x01,0x00 };
const uint8_t GLYPH_2[]     PROGMEM = { 0x21,0x43,0x45,0x49,0x31 };
const uint8_t GLYPH_3[]     PROGMEM = { 0x22,0x41,0x49,0x49,0x36 };
const uint8_t GLYPH_4[]     PROGMEM = { 0x0C,0x14,0x24,0x7F,0x04 };
const uint8_t GLYPH_5[]     PROGMEM = { 0x72,0x51,0x51,0x51,0x4E };
const uint8_t GLYPH_6[]     PROGMEM = { 0x3E,0x49,0x49,0x49,0x06 };
const uint8_t GLYPH_7[]     PROGMEM = { 0x40,0x47,0x48,0x50,0x60 };
const uint8_t GLYPH_8[]     PROGMEM = { 0x36,0x49,0x49,0x49,0x36 };
const uint8_t GLYPH_9[]     PROGMEM = { 0x30,0x49,0x49,0x49,0x3E };

// Largeur (en colonnes) de chaque caractère
uint8_t glyphWidth(char c){
  switch(c){
    case ' ':return sizeof(GLYPH_SPACE);
    case ':':return sizeof(GLYPH_COLON);
    case 'S':return sizeof(GLYPH_S);
    case 'C':return sizeof(GLYPH_C);
    case 'O':return sizeof(GLYPH_O);
    case 'R':return sizeof(GLYPH_R);
    case 'E':return sizeof(GLYPH_E);
    case 'H':return sizeof(GLYPH_H);
    case '0':return sizeof(GLYPH_0);
    case '1':return sizeof(GLYPH_1);
    case '2':return sizeof(GLYPH_2);
    case '3':return sizeof(GLYPH_3);
    case '4':return sizeof(GLYPH_4);
    case '5':return sizeof(GLYPH_5);
    case '6':return sizeof(GLYPH_6);
    case '7':return sizeof(GLYPH_7);
    case '8':return sizeof(GLYPH_8);
    case '9':return sizeof(GLYPH_9);
    default: return sizeof(GLYPH_SPACE);
  }
}

// Récupère l’octet de colonne d’un glyph (colByte = bits des 7 pixels)
uint8_t glyphColByte(char c,uint8_t col){
  switch(c){
    case ' ':return (col<sizeof(GLYPH_SPACE))?pgm_read_byte(&GLYPH_SPACE[col]):0;
    case ':':return (col<sizeof(GLYPH_COLON))?pgm_read_byte(&GLYPH_COLON[col]):0;
    case 'S':return (col<sizeof(GLYPH_S))    ?pgm_read_byte(&GLYPH_S[col])    :0;
    case 'C':return (col<sizeof(GLYPH_C))    ?pgm_read_byte(&GLYPH_C[col])    :0;
    case 'O':return (col<sizeof(GLYPH_O))    ?pgm_read_byte(&GLYPH_O[col])    :0;
    case 'R':return (col<sizeof(GLYPH_R))    ?pgm_read_byte(&GLYPH_R[col])    :0;
    case 'E':return (col<sizeof(GLYPH_E))    ?pgm_read_byte(&GLYPH_E[col])    :0;
    case 'H':return (col<sizeof(GLYPH_H))    ?pgm_read_byte(&GLYPH_H[col])    :0;
    case '0':return (col<sizeof(GLYPH_0))    ?pgm_read_byte(&GLYPH_0[col])    :0;
    case '1':return (col<sizeof(GLYPH_1))    ?pgm_read_byte(&GLYPH_1[col])    :0;
    case '2':return (col<sizeof(GLYPH_2))    ?pgm_read_byte(&GLYPH_2[col])    :0;
    case '3':return (col<sizeof(GLYPH_3))    ?pgm_read_byte(&GLYPH_3[col])    :0;
    case '4':return (col<sizeof(GLYPH_4))    ?pgm_read_byte(&GLYPH_4[col])    :0;
    case '5':return (col<sizeof(GLYPH_5))    ?pgm_read_byte(&GLYPH_5[col])    :0;
    case '6':return (col<sizeof(GLYPH_6))    ?pgm_read_byte(&GLYPH_6[col])    :0;
    case '7':return (col<sizeof(GLYPH_7))    ?pgm_read_byte(&GLYPH_7[col])    :0;
    case '8':return (col<sizeof(GLYPH_8))    ?pgm_read_byte(&GLYPH_8[col])    :0;
    case '9':return (col<sizeof(GLYPH_9))    ?pgm_read_byte(&GLYPH_9[col])    :0;
    default:return 0;
  }
}

// Mesure la largeur totale d’un texte en colonnes (avec 1 colonne d’espace entre chars)
uint16_t measureTextCols(const String&t){
  uint16_t w=0;
  for(uint16_t i=0;i<t.length();i++) w+=glyphWidth(t.charAt(i))+1;
  return (w>0)?(w-1):0; // retire l’espace final
}

// Teste si un pixel texte est allumé pour une colonne globale et une hauteur y (0..6)
bool textPixelOnAt(const String&t,int globalCol,int y){
  if(y<0||y>6) return false;

  int col=0;
  for(uint16_t i=0;i<t.length();i++){
    uint8_t w=glyphWidth(t.charAt(i));
    int charStart=col, charEnd=col+w-1;

    // Colonne située dans ce caractère ?
    if(globalCol>=charStart && globalCol<=charEnd){
      uint8_t glyphCol=(uint8_t)(globalCol-charStart);
      uint8_t colByte=glyphColByte(t.charAt(i),glyphCol);
      return (colByte>>y)&0x01;
    }

    col+=w;

    // Colonne “espace” entre caractères
    if(globalCol==col) return false;
    col+=1;
  }
  return false;
}

// Scroll horizontal du texte (défilement de gauche à droite sur la matrice)
void scrollText(const String&text,uint32_t color,uint16_t speedMs=SCORE_SCROLL_SPEED_MS){
  int totalCols=(int)measureTextCols(text);

  for(int offset=-WIDTH; offset<=totalCols; offset++){
    clearMatrix();

    for(int x=0;x<WIDTH;x++){
      int srcCol=x+offset;
      if(srcCol<0 || srcCol>=totalCols) continue;

      for(int y=0;y<7;y++){
        if(textPixelOnAt(text,srcCol,y)){
          #if TEXT_ROT_180
            int xr=WIDTH-1-x, yr=HEIGHT-1-y;
          #else
            int xr=x, yr=y;
          #endif
          setPixelSafeXY(xr,yr,color);
        }
      }
    }

    pixels.show();
    delay(speedMs);
  }
}

void displayScoreAsText(){
  String s=F("S: ");
  s+=String(score);
  scrollText(s,COLOR_TEXT,SCORE_SCROLL_SPEED_MS);
}

void displayHighScoreText(uint32_t col){
  String s=F("HS: ");
  s+=String(highScore);
  scrollText(s,col,SCORE_SCROLL_SPEED_MS);
}

// =====================================================================================
// Dégradé score (affiche score sous forme de remplissage progressif + animation)
// =====================================================================================
void displayScoreGradient(){
  clearMatrix();

  uint32_t leds=(SCORE_PER_LED==0)?0:(uint32_t)(score/SCORE_PER_LED);
  int maxLeds=(leds<(uint32_t)LED_COUNT)?(int)leds:(int)LED_COUNT;

  if(maxLeds<=0){ pixels.show(); return; }

  int lit=0;
  for(int y=0;y<HEIGHT && lit<maxLeds;y++){
    for(int x=0;x<WIDTH && lit<maxLeds;x++){
      float t=(maxLeds==1)?0.0f:(float)lit/(float)(maxLeds-1);
      uint8_t r=(uint8_t)((1.0f-t)*COLOR_INTENSITY+0.5f);
      uint8_t g=(uint8_t)(t*COLOR_INTENSITY+0.5f);

      setPixelSafeXY(x,y,pixels.Color(r,g,0));
      pixels.show();
      delay(40);
      lit++;
    }
  }
}

// =====================================================================================
// Difficulté : plus le score augmente, plus ça accélère et spawn plus vite
// =====================================================================================
void updateDifficulty(){
  long mv=(long)START_MOVE_MS  - (long)score*(long)MOVE_ACCEL_MS;
  long sp=(long)START_SPAWN_MS - (long)score*(long)SPAWN_ACCEL_MS;

  if(mv<(long)MIN_MOVE_MS)  mv=(long)MIN_MOVE_MS;
  if(sp<(long)MIN_SPAWN_MS) sp=(long)MIN_SPAWN_MS;

  enemyMoveInterval =(unsigned long)mv;
  enemySpawnInterval=(unsigned long)sp;
}

// =====================================================================================
// Célébration record + reset de partie
// =====================================================================================
#define RECORD_FLASH_TIMES 4

void celebrateNewRecord(){
  for(uint8_t i=0;i<RECORD_FLASH_TIMES;i++){
    fillMatrix(COLOR_RECORD); pixels.show(); delay(140);
    clearMatrix();            pixels.show(); delay(120);
  }
}

void resetGame(){
  lost=false;
  score=0;
  updateDifficulty();

  for(uint8_t i=0;i<MAX_ENEMIES;i++){
    enemyActive[i]=false;
    enemyJustSpawned[i]=false;
  }

  playerLives = 0;
  updatePlayerColor();
  bonusActive = false;

  clearMatrix();
  playerY=HEIGHT-1;
  playerLeftX=(WIDTH/2)-1;
  playerRightX=(WIDTH/2);
  drawPlayer();
  needShow=true;

  playStartSound();
}

// =====================================================================================
// Boot helpers :
// - Mode jour si bouton droit maintenu au boot
// - Reset HS si bouton gauche maintenu au boot
// =====================================================================================
inline bool btnLeftPressed();
void applyBrightnessByMode(){
  pixels.setBrightness(isDayMode?BRIGHTNESS_DAY:BRIGHTNESS_NIGHT);
}

void selectModeAtBootWithRightButton(){
  delay(20);
  if(btnRightPressed()){
    delay(200);
    if(btnRightPressed()){
      isDayMode=true;
      Serial.println(F("[MODE] Jour"));
    }
  }
  applyBrightnessByMode();
}

void resetHighScoreIfBootHeld(){
  delay(20);
  if(btnLeftPressed()){
    delay(200);
    if(btnLeftPressed()){
      saveHighScore(0);
      highScore=0;
      Serial.println(F("[HS] Reset au boot"));
    }
  }
}

// Couleur joueur dépend des vies : 0 vie = vert, >=1 vie = bleu
void updatePlayerColor(){
  if(playerLives > 1){
    COLOR_PLAYER = COLOR_PLAYER_3LIVES;   // 2 vies ou plus
  } 
  else if(playerLives == 1){
    COLOR_PLAYER = COLOR_PLAYER_2LIVES;   // 1 vie
  } 
  else {
    COLOR_PLAYER = COLOR_PLAYER_1LIFE;    // 0 vie
  }
}


// =====================================================================================
// SETUP : init pins, pixels, couleurs, RNG, HS, difficulté, etc.
// =====================================================================================
void setup(){
  Serial.begin(9600);

  pinMode(MATRIX_PIN, OUTPUT);
  pinMode(LEFT_BTN_PIN, INPUT_PULLUP);
  pinMode(RIGHT_BTN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  pixels.begin();
  selectModeAtBootWithRightButton();

  // Couleurs de base
  COLOR_BG     = pixels.Color(0,0,0);
  COLOR_PLAYER = pixels.Color(0,COLOR_INTENSITY,0);
  COLOR_ENEMY  = pixels.Color(COLOR_INTENSITY,0,0);
  COLOR_TEXT   = pixels.Color(0,0,COLOR_INTENSITY);
  COLOR_RECORD = pixels.Color(COLOR_INTENSITY,(uint8_t)(COLOR_INTENSITY*0.6f),0);

  COLOR_PLAYER_1LIFE  = pixels.Color(0, COLOR_INTENSITY, 0); // vert
  COLOR_PLAYER_2LIVES = pixels.Color(0, 0, COLOR_INTENSITY); // bleu
  COLOR_PLAYER_3LIVES = pixels.Color(COLOR_INTENSITY, 0, COLOR_INTENSITY); // violet

  // Bonus
  COLOR_BONUS  = pixels.Color(0, 0, COLOR_INTENSITY);

  clearMatrix();
  drawPlayer();
  pixels.show();

  // Init tableaux ennemis
  for(uint8_t i=0;i<MAX_ENEMIES;i++){
    enemyActive[i]=false;
    enemyJustSpawned[i]=false;
  }

  // Seed RNG au boot (puis “reseed” au premier input)
  rng_seedInitial();
  delay(5 + (micros() & 0x3F));
  rng_mix(micros());
  rng_seedFromPool();

  // Gestion HS
  resetHighScoreIfBootHeld();
  highScore = loadHighScore();

  // Init difficulté
  updateDifficulty();

  // Jitter premier spawn (évite un pattern de spawn constant au démarrage)
  lastSpawnAt = millis() - (unsigned long)random(0, enemySpawnInterval);

  // Son boot
  playStartSound();

  #if SERIAL_DEBUG_SCORE_LIVES
    Serial.println(F("[DEBUG] Serial score/vies activé"));
  #endif
}


// =====================================================================================
// LOOP PRINCIPALE
// - Gère les entrées joueur
// - Spawn ennemis / bonus
// - Déplacements synchronisés par timers
// - Collisions
// - Fin de partie, affichages score / HS
// =====================================================================================
void loop(){
  const unsigned long now = millis();

  // -----------------------------------------------------------------
  // Gestion des entrées joueur (avec répétition contrôlée)
  // -----------------------------------------------------------------
  if((now - lastInputAt) >= inputRepeatMs){
    bool L = btnLeftPressed();
    bool R = btnRightPressed();

    // Reseed RNG au premier input réel (anti-patterns au boot)
    if((L || R) && !reseededFromFirstInput){
      rng_mix((uint32_t)micros());
      rng_addAnalogOnce();
      rng_seedFromPool();
      reseededFromFirstInput = true;
    }

    // Déplacement exclusif (pas gauche + droite en même temps)
    if(L && !R){
      movePlayerLeft();
      lastInputAt = now;
    }
    if(R && !L){
      movePlayerRight();
      lastInputAt = now;
    }
  }

  // -----------------------------------------------------------------
  // Spawn des ennemis (si pas en état "lost")
  // -----------------------------------------------------------------
  if(!lost && (now - lastSpawnAt) >= enemySpawnInterval){
    trySpawnEnemy();
    lastSpawnAt = now;
  }

  // -----------------------------------------------------------------
  // Spawn bonus déclenché par le score
  // -----------------------------------------------------------------
  if(!lost){
    trySpawnBonusByScore();
  }

  // -----------------------------------------------------------------
  // Tick principal : déplacement ennemis + bonus + collisions
  // -----------------------------------------------------------------
  if(!lost && (now - lastMoveAt) >= enemyMoveInterval){
    stepEnemies();
    stepBonus();
    checkCollision();
    drawPlayer();
    lastMoveAt = now;

    // ---------------------------------------------------------------
    // FIN DE PARTIE
    // ---------------------------------------------------------------
    if(lost){
      newRecordJustSet = false;
      updateHighScoreIfNeeded();

      // Logs série (debug humain)
      Serial.print(F("Score: "));
      Serial.println(score);
      Serial.print(F("High Score: "));
      Serial.println(highScore);

      playLoseSound();

      // Animation de mort
      flashBigX(3);

      delay(750);
      // Affichage score texte
      displayScoreAsText();

      // High Score
      if(newRecordJustSet){
        celebrateNewRecord();
        playNewRecordSound();
        displayHighScoreText(COLOR_RECORD);
        displayHighScoreText(COLOR_RECORD); // double affichage = plus lisible
      } else {
        displayHighScoreText(COLOR_TEXT);
      }

      // Affichage graphique du score
      displayScoreGradient();
    }
  }

  // -----------------------------------------------------------------
  // Affichage consolidé (un seul pixels.show() par frame si possible)
  // -----------------------------------------------------------------
  if(needShow){
    pixels.show();
    needShow = false;
  }

  // -----------------------------------------------------------------
  // DEBUG série sans spam (uniquement si changement)
  // -----------------------------------------------------------------
  #if SERIAL_DEBUG_SCORE_LIVES
    static long lastSerialScore = -1;
    static int  lastSerialLives = -1;

    if(score != lastSerialScore || (int)playerLives != lastSerialLives){
      Serial.print(F("[GAME] Score="));
      Serial.print(score);
      Serial.print(F(" | Vies="));
      Serial.println(playerLives);
      lastSerialScore = score;
      lastSerialLives = (int)playerLives;
    }
  #endif

  // -----------------------------------------------------------------
  // Reset de la partie après défaite
  // (bouton gauche maintenu)
  // -----------------------------------------------------------------
  if(lost && btnLeftPressed()){
    delay(30);                 // anti-rebond simple
    while(btnLeftPressed()){}  // attend relâchement
    resetGame();
  }
}
