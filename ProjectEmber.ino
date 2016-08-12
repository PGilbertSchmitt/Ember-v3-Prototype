#include <EEPROMVar.h>
#include <EEPROMex.h>
#include <Adafruit_VS1053.h>

#define BUTTON_PIN		2	// Reset button pin location
#define LED_PIN			13	// LED for testing

#define HEAD_LOC		0	// Location of header information in EEPROM
#define COUNT_LOC		2	// Location of count information in EEPROM
#define START_LOC		4	// Location of where timecards begin in EEPROM
#define MAX_CARD_COUNT	12	// Number of cards stored in EEPROM (Pretend that there are only 64 bytes to play with)
#define EEPROM_SIZE		64	// Number of bytes available in EEPROM
#define CARD_SIZE		5	// Number of bytes taken up by a block
#define MAX_TIME		600	// Number of (1/10th) seconds until time reset

#define WEIGHT_THRESHOLD	50	// Weight (in pounds) to receive positive strain reading

#define RESET_PIN		-1		// Unused pin
#define CS_PIN			7		// Chip select
#define DCS_PIN			6		// Data/command select
#define CARDCS_PIN		4		// SD Card chip select
#define DATA_REQ_PIN	3		// Data request interrupt

#define QUARTET_1 		255		// Bit mask byte 1
#define QUARTET_2 		65280	// Bit mask byte 2
#define QUARTET_3 		16711680 // Bit mask byte 3

struct uint24_t {
	// bytes go in order determined by the EEPROMex function writeBlock()
	byte b3;	// most significant
	byte b2;
	byte b1;	// least significant
};

struct timeCard 
{
	uint24_t timeIn;		// Tenths of a second since midnight
	uint16_t duration;		// Tenths of a second since timeIn
};

enum playState
{
	PLAYING = 0,
	DONE,
	NOPLAY,
	START
};

// Two known strain readings are used to extrapolate the weight

float load_A = 0.0;		// lbs
float reading_A = 349.0;

float load_B = 135.0;	// lbs
float reading_B = 266.0;

// Used to store current data between loops
uint32_t timeIn;
uint32_t timeOut;
unsigned long millisIn;
unsigned long timeSince;
const struct timeCard emptyCard = {{0,0,0},0}; 
struct timeCard currentCard;

playState state;
bool cardSet;

bool buttonDown;
bool wasDown;
uint32_t buttonTimeIn;
uint32_t buttonTimeOut;

uint16_t storedCards;
uint16_t headAddress;

const char *trackName = "track001.mp3";

Adafruit_VS1053_FilePlayer player = 
	Adafruit_VS1053_FilePlayer
	(
		RESET_PIN,
		CS_PIN,
		DCS_PIN,
		DATA_REQ_PIN,
		CARDCS_PIN
	);

void setup() 
{
	pinMode(BUTTON_PIN, INPUT);
	pinMode(LED_PIN, OUTPUT);
	Serial.begin(9600);
	while(!Serial){} // Wait until serial communication can begin

	// Music Player setup
	if (!player.begin()) {
		Serial.println(F("No VS1053"));
		while (1);
	}
	Serial.println(F("VS1053 found"));
	player.setVolume(20, 20);
	player.useInterrupt(VS1053_FILEPLAYER_PIN_INT); // Data request interrupt

	// Initialize SD card
	SD.begin(CARDCS_PIN);

	storedCards = EEPROM.readInt(COUNT_LOC);
	headAddress = EEPROM.readInt(HEAD_LOC); // Tells us where the first card is (or would be) in EEPROM

	cardSet = false;

	state = NOPLAY;

	buttonDown = false;
	wasDown = false;
	
	delay(50);
}

void loop() 
{
	uint32_t sec = millis() / 100;	// Get the current time in tenths of a second
	sec %= MAX_TIME;
	
	Serial.print("Time is: ");
	Serial.print(sec);
	Serial.print("\tThe state is: ");
	switch(state) {
		case PLAYING: 	Serial.println("PLAYING"); break;
		case DONE: 		Serial.println("DONE"); break;
		case NOPLAY: 	Serial.println("NOPLAY"); break;
		case START: 	Serial.println("START"); break;
	}
	// PLAYING MUSIC

	
	switch (state) {
		case PLAYING	:
		// Wait until we have to stop playing
			timeSince = millis() - millisIn;
			timeSince /= 100;
			if (timeSince > (unsigned long)currentCard.duration){
				state = DONE;
			} else {
				// If the track is over, start it again
				if (player.stopped()) {
					player.startPlayingFile(trackName);
				}
			}
			break;
		case DONE	:	
		// Stop the music, update the currentCard and header information, then move on
			removeCard();
			timeIn = 0;
			timeSince = 0;
			currentCard = emptyCard;
			
			state = NOPLAY;
			cardSet = false;

			digitalWrite(LED_PIN, LOW);
			player.stopPlaying();
			
			break;
		case NOPLAY	:
		// Wait until we have a card (and can play)
			if (storedCards > 0 && !cardSet) {	// Only moves on if there is a card to play
				Serial.println("Card set");
				EEPROM.readBlock(headAddress, currentCard);	// currentCard is now the new card
				timeIn = blockToLong(currentCard.timeIn);
				//timeOut = timeIn + (uint32_t)currentCard.duration;
				cardSet = true;
			}

			if (cardSet) {
				Serial.print(sec);
				Serial.print(" ~ ");
				Serial.println(timeIn);
				if (sec > timeIn) {
					state = START;
				}
			}
			
			break;
		case START	:	// Play the music
			state = PLAYING;
			millisIn = millis(); // the time when we start playing according to the actual clock
			//Emergency readouts
			//Serial.print("Starting card from address ");
			//Serial.print(headAddress);
			//Serial.print(" with ");
			//Serial.print(storedCards);
			//Serial.println(" cards in store");
			//Serial.print("Card: ");
			//Serial.print(timeIn);
			//Serial.print(", ");
			//Serial.println(timeOut);
			digitalWrite(LED_PIN, HIGH);
			player.startPlayingFile(trackName);
			
			break;
		default:
			// Just in case I suck more than I think I do
			state = NOPLAY;
	};

	// CHECKING INPUT

	// Read button input here through custom get method
	getSeatState(buttonDown);
	

	if (buttonDown && !wasDown) {	// Just sat down; record time
		buttonTimeIn = millis() / 100;
		Serial.print("Time in:\t");
		Serial.println(buttonTimeIn);

		wasDown = true;
	}

	if (!buttonDown && wasDown) {	// Just sat down, record time and set new card
		buttonTimeOut = sec;
		timeCard newCard;
		
		newCard.timeIn = longToBlock(buttonTimeIn);
		if (buttonTimeOut > buttonTimeIn) {
			newCard.duration = buttonTimeOut - buttonTimeIn;
		} else {
			newCard.duration = (MAX_TIME - buttonTimeIn) + buttonTimeOut;
		}
		Serial.print("Duration:\t");
		Serial.println(newCard.duration);

		// New card gets added here
		addCard(newCard);
		wasDown = false;
	}
	
	delay(100);
}

bool isTCEqual(const timeCard &first, const timeCard &second) {
	if (first.timeIn.b1 != second.timeIn.b1) {return false;}
	if (first.timeIn.b2 != second.timeIn.b2) {return false;}
	if (first.timeIn.b3 != second.timeIn.b3) {return false;}
	if (first.duration != second.duration) {return false;}
	return true;
}

void getSeatState(bool &seatState) {
	// This is the simple button code
	if (digitalRead(BUTTON_PIN) == HIGH) {
		seatState = true;
	} else {
		seatState = false;
	}

	/*// This is the strain gauge version (the final (it works))
	float reading_Now = analogRead(0);  // analog in 0 for Strain 1
  
	// Calculate load by interpolation 
	float load_Now = ((load_B - load_A)/(reading_B - reading_A)) 
						* (reading_Now - reading_A) + load_A;

	if (load_Now > WEIGHT_THRESHOLD) {
		seatState = true;
	} else {
		seatState = false;
	}*/
}

void removeCard () {
	storedCards--;

	headAddress += CARD_SIZE;
	if (headAddress > EEPROM_SIZE) {
		headAddress = START_LOC;
	}
	
	EEPROM.updateInt(COUNT_LOC, storedCards);
	EEPROM.updateInt(HEAD_LOC, headAddress);

	// Don't worry about zero-ing out the bytes in EEPROM. That uses up read/writes.
	// Instead, whether a byte is important is determined by header and count
}

void addCard (const timeCard &card) {
	if (storedCards < MAX_CARD_COUNT) { // Only do this if we have room
		int cardLocation = headAddress + (CARD_SIZE * storedCards);
		if (cardLocation >= EEPROM_SIZE) {
			cardLocation -= (EEPROM_SIZE - START_LOC);
		}

		storedCards++;
		EEPROM.update(COUNT_LOC, storedCards);

		EEPROM.updateBlock(cardLocation, card);
	}
}

// Conversion from uint24_t to uint32_t
uint32_t blockToLong(const uint24_t & block) {
	return block.b1 + ((uint16_t)block.b2 << 8) + ((uint32_t)block.b3 << 16);
}

// Conversion from uint32_t to uint24_t
uint24_t longToBlock(const uint32_t & longNum) {
	uint24_t block;
	block.b1 =  longNum & QUARTET_1;
	block.b2 = (longNum & QUARTET_2) >> 8;
	block.b3 = (longNum & QUARTET_3) >> 16;
	return block;
}















