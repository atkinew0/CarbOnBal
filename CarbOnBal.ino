// This software, known as CarbOnBal is
// Copyright, 2017-2020 L.L.M. (Dennis) Meulensteen. dennis@meulensteen.nl
//
// This file is part of CarbOnBal. A combination of software and hardware.
// I hope it may be of some help to you in balancing your carburetors and throttle bodies.
// Always be careful when working on a vehicle or electronic project like this.
// Your life and health are your sole responsibility, use wisely.
//
// CarbOnBal hardware is covered by the CERN Open Hardware License v1.2
// a copy of the text is included with the source code.
//
// CarbOnBal is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// CarbOnBal is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with CarbOnBal.  If not, see <http://www.gnu.org/licenses/>.

#include <Arduino.h>
#include <EEPROM.h>

#include "functions.h"
#include "globals.h"
#include LANGUAGE
#include "lang_generic.h"
#include "lcdWrapper.h"
#include "menu.h"
#include "menuActions.h"
#include "utils.h"
settings_t settings;

// The software uses a lot of global variables. This may not be elegant but its one way of writing non-blocking code
int inputPin[NUM_SENSORS] = { A0, A1, A2, A3 }; //used as a means to access the sensor pins using a counter
int timeBase = 0; //allows us to calculate how long it took to measure and update the display (only used for testing)
long sums[NUM_SENSORS] = { 0, 0, 0, 0 }; //tracks totals for calculating a numerical average

bool freezeDisplay = false; //used to tell when a user wants to freeze the display
unsigned int rpm;                                       //stores the current RPM

int readingCount[NUM_SENSORS]; //used to store the number of captured readings for calculating a numerical average
int previousValue[NUM_SENSORS]; //stores the previous sensor value to determine ascending v. descending pressures
unsigned int average[NUM_SENSORS]; //used to share the current average for each sensor
int ambientPressure; //stores current ambient pressure for negative pressure display
unsigned long lastUpdate;
int packetRequestCount = 0;


volatile longAverages avg[NUM_SENSORS];

uint8_t labelPosition = 0;

unsigned int serialValues[] = { 0, 0, 0, 0 };
unsigned long packetCounter = 0;
unsigned long startTime;
bool useLaptopForDataInput = false;
volatile unsigned long lastInterrupt = micros();
volatile unsigned long periodUs = 0;
volatile unsigned long interruptDurationUs = 0;
volatile bool isSerialAllowed = true;
bool dataDumpMode = false;


//this does the initial setup on startup.
void setup() {
	lcd_begin(DISPLAY_COLS, DISPLAY_ROWS);

	loadSettings();                 //load saved settings into memory from FLASH
	Serial.begin(getBaud(settings.baudRate));

	setInputActiveLow(SELECT);          //set the pins connected to the switches
	setInputActiveLow(LEFT);             //switches are wired directly and could
	setInputActiveLow(RIGHT);                //short out the pins if setup wrong
	setInputActiveLow(CANCEL);

	analogWrite(brightnessPin, settings.brightness);  //brightness is PWM driven
	analogWrite(contrastPin, settings.contrast); //contrast is PWM with smoothing (R/C network) to prevent flicker

	ambientPressure = detectAmbient(); //set ambient pressure (important because it varies with weather and altitude)

	if (settings.splashScreen) {
		doMatrixDemo();

		lcd_setCursor(4, 1);
		lcd_print(txtWelcome);
		delay(2000);

		lcd_clear();
		doAbsoluteDemo();
		doRelativeDemo();
	}

	if(!useLaptopForDataInput){
		//set timer1 interrupt at 1Hz
		  TCCR1A = 0;// set entire TCCR1A register to 0
		  TCCR1B = 0;// same for TCCR1B
		  TCNT1  = 0;//initialize counter value to 0
		  // set compare match register for 1hz increments
		  OCR1A = 249;// = (16*10^6) / (1*1024) - 1 (must be <65536)
		  // turn on CTC mode
		  TCCR1B |= (1 << WGM12);
		  // Set CS10 and CS11 bits for 64 prescaler
		  TCCR1B |= (1 << CS11) | (1 << CS10);
		  // enable timer compare interrupt
		  TIMSK1 |= (1 << OCIE1A);
		setInterrupt(true);
	}

}

void doSerialRead() {
	unsigned int readInt;

	// request a number of consecutive packets to populate the built in hardware serial cache
	while (packetRequestCount <= 15) {
		Serial.write(REQUEST_PACKET); //request a packet
		packetRequestCount++;
	}
	for(int i=0; i<10; i++){//max ten attempts to read a valid packet to avoid hanging up the UI

		if ((Serial.available() > 8) && (Serial.read() == START_PACKET)) {

			for (int i = 0; i < 4; i++) {
				readInt = ((uint8_t) Serial.read()) << 7; //only using lower 7 bits for payload!
				readInt |= ((uint8_t) Serial.read());
				serialValues[i] = readInt;
			}
			packetCounter++;
			packetRequestCount--;
			break;
		}
	}
}

void loop() {
	startTime = micros();

	if(useLaptopForDataInput) doSerialRead();

	switch (buttonPressed()) {					//test if a button was pressed
	case SELECT:
		actionDisplayMainMenu();
		break;        //the menu is the only function that does not return asap
	case LEFT:
		if (settings.button1 == 0) { // there are two modes for this pin, user settable
			actionContrast();
		} else {
			resetAverages();
		}
		break;
	case RIGHT:
		if (settings.button2 == 0) { // there are two modes for this pin, user settable
			actionBrightness();
		} else {
			doRevs();
		}
		break;

	case CANCEL:
		freezeDisplay = !freezeDisplay;
		break; //toggle the freezeDisplay option
	}

	if (!freezeDisplay
			&& (settings.graphType == 2 || (millis() - lastUpdate) > 100)) {//only update the display every 100ms or so to prevent flickering

		switch (settings.graphType) { //there are two types of graph. bar and centered around the value of the master carb
		case 0:
			lcdBarsSmooth(average);
			lastUpdate = millis();
			break;          //these functions both draw a graph and return asap
		case 1:
			lcdBarsCenterSmooth(average);
			lastUpdate = millis();
			break;    //
		case 2: //in diagnostic mode we output the values via the serial port for display or analysis on a PC

			if (isSerialAllowed && settings.arduinoCompatible) {
				serialOut(average);
			} else if (isSerialAllowed ) {
				serialOutBytes(average);
			}
			isSerialAllowed = false;

			if ((millis() - lastUpdate) > 100) {
				lcdDiagnosticDisplay(average);
				lastUpdate = millis();
			}
			break;
		}

	} else if (freezeDisplay) {
		//show a little snow flake to indicate the frozen state of the display
		drawSnowFlake();
	}
	timeBase = micros() - startTime; //monitor how long it takes to traverse the main loop
}


// Alternative basic algorithm using only long integer arithmetic
// now the definitive algorithm
void intRunningAverage() {
	unsigned int value;
	int factor = settings.emaFactor;

	for (int sensor = 0; sensor < settings.cylinders; sensor++) { //loop over all sensors
		value = (unsigned int) readSensorCalibrated(sensor);
		avg[sensor].longVal = longExponentialMovingAverage( factor, avg[sensor].longVal, value);
		average[sensor] = avg[sensor].intVal[1];
	}
}


/*
 * uses the average ms between calls to this function as a trigger as a measure of RPM
 * To the actual time and returns false if there is more than 25% difference
 * between the averaged value and the current value. This happens mainly when revving the engine
 * it works because the average values trail the actual values allowing the difference to accrue
 * just take care to only call it from a single well-defined place
 */

bool stableRPM = true;
long averageDeltaMs;
unsigned long previousTriggerTime;
unsigned long unstableTime;

bool isRPMStable(int sensor) {
	unsigned long triggerTime = millis();
	unsigned long deltaMs;
	int shift = 16;
	unsigned int unshiftedAverage;
	unsigned int hysteresis;

	if (sensor == 0) {
		deltaMs = triggerTime - previousTriggerTime;
		unshiftedAverage = averageDeltaMs >> shift;
		hysteresis = unshiftedAverage >> settings.emaRpmSensitivity; // >>2 = 25% difference in RPM from the average

		if ((deltaMs > (unshiftedAverage + hysteresis))
				|| (deltaMs < (unshiftedAverage - hysteresis))) {
			stableRPM = false;
			unstableTime = triggerTime;
		} else if (triggerTime - unstableTime > 500) {
			stableRPM = true;
		}

		//todo fix up
		//averageDeltaMs = longExponentialMovingAverage(shift, 12, averageDeltaMs, deltaMs);

		previousTriggerTime = triggerTime;
	}

	return stableRPM;
}

//only measures the vacuum 'suck'(intake) not the release(compression, work & exhaust) phase, this greatly simplifies the logic and
// shouldn't affect the average negatively
void descendingAverage() {
	int value;
	int factor = settings.emaFactor;

	for (int sensor = 0; sensor < settings.cylinders; sensor++) { //loop over all sensors
		value = readSensorCalibrated(sensor);
		if (value < previousValue[sensor]) { // descending pressures only
			sums[sensor] += value;
			readingCount[sensor]++; //keeps count of the number of readings collected
		} else {                       //above the previous value or equal to it
			if (readingCount[sensor] > 1) {
				//calculate the average now we've measured a whole "vacuum signal flank"
				avg[sensor].longVal = longExponentialMovingAverage( factor, avg[sensor].longVal, sums[sensor] / readingCount[sensor]);
				average[sensor] = avg[sensor].intVal[1];
			}
			//done the calculations, now reset everything ready to start over when the signal drops again
			sums[sensor] = 0;
			readingCount[sensor] = 0;
		}
		previousValue[sensor] = value;
	}
}

// display centered bars, centered on the reference carb's reading, because that's the target we are aiming for
//takes an array of current average values for all sensors as parameter
void lcdBarsCenterSmooth(unsigned int value[]) {
	const uint8_t segmentsInCharacter = 5; //we need this number to make the display smooth

	byte bar[4][8];                         //store one custom character per bar

	char bars[DISPLAY_COLS + 1];          //store for the bar of full characters

	unsigned int maximumValue = maxVal(value); //determine the sensor with the highest average
	unsigned int minimumValue = minVal(value); //determine the lowest sensor average
	int range;          //store the range between the highest and lowest sensors
	int zoomFactor;                              //store the zoom of the display

	const uint8_t hysteresis = 4;

	//the range depends on finding the reading farthest from the master carb reading
	if (maximumValue - value[settings.master - 1]
			>= value[settings.master - 1] - minimumValue) {
		range = maximumValue - value[settings.master - 1];
	} else {
		range = value[settings.master - 1] - minimumValue;
	}

	//sets the minimum range before the display becomes 'pixelated' there are 100 segments available, 50 on either side of the master
	int ranges[] = { 50, 100, 150, 300, 512 };
	if (range < ranges[settings.zoom]) {
		range = ranges[settings.zoom];
	}

	zoomFactor = range / 50;

	for (int sensor = 0; sensor < settings.cylinders; sensor++) { //for each of the sensors the user wants to use
		int delta = value[sensor] - value[settings.master - 1]; //find the difference between it and master
		int TotalNumberOfLitSegments = delta / zoomFactor; //determine the number of lit segments
		int numberOfLitBars = TotalNumberOfLitSegments / segmentsInCharacter; //determine the number of whole characters
		int numberOfLitSegments = TotalNumberOfLitSegments
				% segmentsInCharacter;   //determine the remaining stripes

		if (sensor != settings.master - 1) { //for all sensors except the master carb sensor
			makeCenterBars(bars, numberOfLitBars); //give us the bars of whole characters
			lcd_setCursor(0, sensor);
			lcd_print(bars);                     //place the bars in the display

			makeChar(bar[sensor], numberOfLitSegments); //make a custom char for the remaining stripes
			lcd_createChar(sensor + 2, bar[sensor]);

			if (numberOfLitSegments > 0) {
				lcd_setCursor(10 + numberOfLitBars, sensor); //place it on the right
			} else {
				lcd_setCursor(9 + numberOfLitBars, sensor); //or in the center
			}
			if (numberOfLitBars < 10)
				lcd_write(byte(sensor + 2));

			//hysteresis gives the display more stability and prevents the labels from flipping from side to side constantly.
			if (!settings.silent) {
				if (numberOfLitBars < -hysteresis)
					labelPosition = 15;
				if (numberOfLitBars > hysteresis)
					labelPosition = 0;
				printLcdSpace(labelPosition, sensor, 5);
				lcd_printFormatted(differenceToPreferredUnits(delta)); //display the difference between this sensor and master
			}
		} else {
			float result = convertToPreferredUnits(value[sensor],
					ambientPressure);
			printLcdSpace(0, sensor, 5);
			lcd_printFormatted(result); //print the absolute value of the reference carb
			printLcdInteger(timeBase, 10, sensor, 4); //show how long it took to measure four sensors
			printLcdSpace(15, sensor, 5);
			lcd_printFormatted(differenceToPreferredUnits(range * 2)); //show the zoom range
		}
	}
}

// this is used to display four plain non-zoomed bars with absolute pressure readings
void lcdBarsSmooth(unsigned int value[]) {
	const uint8_t segmentsInCharacter = 5;
	const uint8_t hysteresis = 4;

	byte bar[4][8];
	char bars[DISPLAY_COLS + 1];

	for (int sensor = 0; sensor < settings.cylinders; sensor++) {
		int TotalNumberOfLitSegments = 100000L / 1024 * value[sensor] / 1000; // integer math works faster, so we multiply by 1000 and divide later, powers of two would be even faster
		int numberOfLitBars = TotalNumberOfLitSegments / segmentsInCharacter;
		int numberOfLitSegments = TotalNumberOfLitSegments
				% segmentsInCharacter;

		makeBars(bars, numberOfLitBars, 0); //skip function probably no longer needed
		lcd_setCursor(0, sensor);
		lcd_print(bars);

		makeChar(bar[sensor], numberOfLitSegments);
		lcd_createChar(sensor + 2, bar[sensor]);
		lcd_setCursor(numberOfLitBars, sensor);
		lcd_write(byte(sensor + 2));

		if (!settings.silent) {
			if (numberOfLitBars < 10 - hysteresis)
				labelPosition = 14;
			if (numberOfLitBars > 10 + hysteresis)
				labelPosition = 0;
			printLcdSpace(labelPosition, sensor, 5);
			float result = convertToPreferredUnits(value[sensor],
					ambientPressure);
			lcd_printFormatted(result);
		}
	}
}

void lcdDiagnosticDisplay(unsigned int value[]) {
	for (int sensor = 0; sensor < settings.cylinders; sensor++) {
		float result = convertToPreferredUnits(value[sensor], ambientPressure);
		printLcdSpace(0, sensor, 5);
		lcd_printFormatted(result);		//raw value

		printLcdSpace(8, sensor, 5);
		int delta = value[sensor] - value[settings.master - 1];
		lcd_printFormatted(differenceToPreferredUnits(delta));  //delta value
	}
	printLcdInteger(timeBase, 15, 0, 5); //time base
	printLcdInteger(periodUs, 15, 1, 5); //interrupt freq in uS
	//printLcdInteger(stableRPM, 15, 2, 5); //stable RPM
	printLcdInteger(interruptDurationUs, 15, 2, 5);
	printLcdInteger(packetCounter, 15, 3, 5); //serial packetrs sent

}

//saves our settings struct
void actionSaveSettings() {
	EEPROM.put(0, versionUID);  //only saves changed bytes!
	EEPROM.put(settingsOffset, settings);  //only saves changed bytes!
}

//loads the settings from EEPROM (Flash)
void loadSettings() {
	uint8_t compareVersion = 0;
	EEPROM.get(0, compareVersion);

	if (compareVersion == versionUID) { //only load settings if saved by the current version, otherwise reset to 'factory' defaults
		EEPROM.get(settingsOffset, settings); //settings are stored immediately after the version UID
	} else {
		resetToFactoryDefaultSettings();
	}

	doContrast(settings.contrast);
	doBrightness(settings.brightness);
}

//does the display while clearing the calibration array
void doZeroCalibrations() {
	lcd_clear();
	lcd_setCursor(3, 1);
	lcd_print(txtWiping);
	zeroCalibrations();
	doConfirmation();
}

//determine where the calibration value is stored in EEPROM depending on the sample value
int getCalibrationTableOffsetByValue(int sensor, int value) {
	return calibrationOffset + ((sensor - 1) * numberOfCalibrationValues)
			+ (value >> 2);
}

//determine where in EEPROM the calibration value is stored depending on the position
int getCalibrationTableOffsetByPosition(int sensor, int pos) {
	return calibrationOffset + ((sensor - 1) * numberOfCalibrationValues) + pos;
}

//only write if the value needs writing (saves write cycles)
void eepromWriteIfChanged(int address, int8_t data) {
	if ((uint8_t) data != EEPROM.read(address)) {
		EEPROM.write(address, (uint8_t) data); 		//write the data to EEPROM
	}
}

int readSensorRaw(int sensor) {
	//dummy read did nothing measurable, skip that nonsense
	return (analogRead(inputPin[sensor]));
}
int readSensorCalibrated(int sensor) {

	//if(useLaptop) return (int) serialValues[sensor];

	int value = readSensorRaw(sensor);
	if (sensor > 0) { //only for the calibrated sensors, not the master
		value += (int8_t) EEPROM.read(
				getCalibrationTableOffsetByValue(sensor, value)); //adds this reading adjusted for calibration
	}
	return value;
}

//clear the flash for a single sensor
void doClearCalibration(int sensor) {
	for (int i = 0; i < numberOfCalibrationValues; i++) {
		eepromWriteIfChanged(getCalibrationTableOffsetByPosition(sensor, i), 0); //write the data directly to EEPROM
	}
}

//actually clears the flash for all the sensors
void zeroCalibrations() {
	for (uint8_t sensor = 1; sensor < (NUM_SENSORS); sensor++) {
		doClearCalibration(sensor);
	}
}

void doClearCalibration1() {
	doClearCalibration(1);
	doConfirmation();
}
void doClearCalibration2() {
	doClearCalibration(2);
	doConfirmation();
}
void doClearCalibration3() {
	doClearCalibration(3);
	doConfirmation();
}

void doViewCalibration1() {
	doViewCalibration(1);
}
void doViewCalibration2() {
	doViewCalibration(2);
}
void doViewCalibration3() {
	doViewCalibration(3);
}

void doCalibrate1() {
	doCalibrate(1);
}
void doCalibrate2() {
	doCalibrate(2);
}
void doCalibrate3() {
	doCalibrate(3);
}
void doCalibrate(int sensor) {
	const int shift = 5;
	const int factor = 4;
	int maxValue = -127;
	int minValue = 127;
	int lowestCalibratedValue = 1024;
	int readingStandardPre, readingSensor, readingStandardPost;
	setInterrupt(false);

	lcd_clear();

	lcd_setCursor(0, 0);
	lcd_print(txtCalibrationBusy);

	lcd_setCursor(0, 1);
	lcd_print(txtCalibrationBusy2);

	lcd_setCursor(0, 3);
	lcd_print(txtPressAnyKey);
	displayKeyPressPrompt();

	delay(500); //otherwise key still pressed, probably need a better solution

	//initialize temp values array, note full ints (16 bits) used
	int values[numberOfCalibrationValues];

	//read existing values from EEPROM and pre-shift them
	//shifting an int left by n bits simply gives us n bits of 'virtual' decimal places
	// this is needed for accuracy because EMA calculation works by adding or subtracting relatively small values
	// which would otherwise all be truncated to '0'
	for (int i = 0; i < numberOfCalibrationValues; i++) {
		values[i] = (int8_t) EEPROM.read(
				getCalibrationTableOffsetByPosition(sensor, i));
		values[i] <<= shift;
	}

	while (!buttonPressed()) {
		readingStandardPre = readSensorRaw(0);  //read master
		readingSensor = readSensorRaw(sensor); //read calibration sensor
		readingStandardPost = readSensorRaw(0);  //read master again

		int readingStandard = (readingStandardPre + readingStandardPost) >> 1; //average both to increase accuracy on slopes
		int calibrationValue = readingStandard - readingSensor;

		//record some basic quality statistics
		if (calibrationValue > maxValue)
			maxValue = calibrationValue;
		if (calibrationValue < minValue)
			minValue = calibrationValue;
		if (readingSensor < lowestCalibratedValue)
			lowestCalibratedValue = readingSensor;

		values[(readingSensor >> 2)] = intExponentialMovingAverage(shift,
				factor, values[(readingSensor >> 2)], calibrationValue);
	}

	//post_shift the values in preparation of writing back to EEPROM
	// we don't need to save the 'decimal places' because they are not needed anymore.
	// so we lose them by shifting them out of range to the right
	for (int i = 0; i < numberOfCalibrationValues; i++) {
		values[i] >>= shift;
	}

	//save calibrations
	for (int i = 0; i < numberOfCalibrationValues; i++) {
		eepromWriteIfChanged(getCalibrationTableOffsetByPosition(sensor, i),
				(int8_t) values[i]);
	}

	lcd_clear();

	lcd_setCursor(0, 0);
	lcd_print(txtCalibrationDone);

	lcd_setCursor(0, 1);
	lcd_print(txtLowestPressure);
	printLcdInteger(lowestCalibratedValue, 15, 1, 4);
	lcd_setCursor(0, 2);
	lcd_print(txtMinAdjust);
	printLcdInteger(minValue, 16, 2, 3);
	lcd_setCursor(0, 3);
	lcd_print(txtMaxAdjust);
	printLcdInteger(maxValue, 16, 3, 3);

	waitForAnyKey();
	displayCalibratedValues(values);
	setInterrupt(true);
}

void doViewCalibration(int sensor) {
	setInterrupt(false);
	int values[numberOfCalibrationValues];

	for (int i = 0; i < numberOfCalibrationValues; i++) {
		values[i] = (int8_t) EEPROM.read(
				getCalibrationTableOffsetByPosition(sensor, i));
	}

	displayCalibratedValues(values);
	setInterrupt(true);
}

//display indicator arrows and numeric offsets so we don't get lost in the graph of calibration values.
void displayNavArrowsAndOffsets(int valueOffset,
bool topLeftArrowPositionAvailable,
bool topRightArrowPositionAvailable) {
	if (valueOffset == 0) {
		(topLeftArrowPositionAvailable) ?
				lcd_setCursor(0, 0) : lcd_setCursor(0, 3);
		lcd_printChar('[');
		lcd_printInt(valueOffset);
	}
	if (valueOffset > 0) {
		(topLeftArrowPositionAvailable) ?
				lcd_setCursor(0, 0) : lcd_setCursor(0, 3);
		lcd_printChar(char(MENUCARET + 1)); //little arrow to the left
		lcd_printInt(valueOffset);
	}
	if (valueOffset == numberOfCalibrationValues - 20) {
		(topRightArrowPositionAvailable) ?
				lcd_setCursor(16, 0) : lcd_setCursor(16, 3);
		lcd_printInt(valueOffset + 20);
		lcd_printChar(']');
	}
	if (valueOffset < numberOfCalibrationValues - 20) {
		(topRightArrowPositionAvailable) ?
				lcd_setCursor(16, 0) : lcd_setCursor(16, 3);
		if ((valueOffset + 20) < 100)
			lcd_printChar(' ');

		lcd_printInt(valueOffset + 20);
		lcd_printChar(char(MENUCARET)); //little arrow to the right
	}
}

// Show a graph of the computed calibration values so the user can get an idea of the quality of the sensors
// and of the calibration. Repeated calibration increases the accuracy.
// Note: if all sensors are showing the same type of displacement that means that sensor 0
// is off by that much in the opposite direction.
void displayCalibratedValues(int values[]) {
	int valueOffset = 0;
	int numberOfColumns = 20;
	int segmentsPerCharacter = 8;
	int numberOfCharacters = 4;
	int numberOfSegments = segmentsPerCharacter * (numberOfCharacters / 2);
	int valuePerSegment = settings.calibrationMax / numberOfSegments; //128 / 16 = 8
	int pressedButton = 0;
	bool dataChanged = true;
	bool topLeftArrowPositionAvailable, topRightArrowPositionAvailable = true;
	makeCalibrationChars();

	while (pressedButton != CANCEL) {
		if (dataChanged) {
			lcd_clear();
			for (int column = 0; column < numberOfColumns; column++) {
				int valueInSegments = values[valueOffset + column]
						/ valuePerSegment;

				if (valueInSegments <= segmentsPerCharacter
						&& valueInSegments > 0) {
					lcd_setCursor(column, 1);
					lcd_write(byte((byte) 8 - valueInSegments));
				} else if (valueInSegments > 2 * segmentsPerCharacter) {
					lcd_setCursor(column, 0);
					lcd_printChar('|');
					lcd_setCursor(column, 1);
					lcd_printChar('|');
				} else if (valueInSegments > segmentsPerCharacter) {
					lcd_setCursor(column, 0);
					lcd_write(
							byte(
									(byte) 8
											- (valueInSegments
													% segmentsPerCharacter)));
				} else if (valueInSegments < (2 * -segmentsPerCharacter)) {
					lcd_setCursor(column, 2);
					lcd_printChar('|');
					lcd_setCursor(column, 3);
					lcd_printChar('|');
				} else if (valueInSegments < 0
						&& (valueInSegments >= -segmentsPerCharacter)) {
					lcd_setCursor(column, 2);
					lcd_write(byte((byte) (-valueInSegments) - 1));
				} else if (valueInSegments < 0
						&& (valueInSegments < -segmentsPerCharacter)) {
					lcd_setCursor(column, 3);
					lcd_write(
							byte(
									(byte) 8
											- ((valueInSegments + 1)
													% segmentsPerCharacter)));
				}

				if (column == 0)
					topLeftArrowPositionAvailable = (valueInSegments <= 0);
				if (column == 19)
					topRightArrowPositionAvailable = (valueInSegments <= 0);

			}

			//show arrows to indicate scrolling and our location in the calibration array
			displayNavArrowsAndOffsets(valueOffset,
					topLeftArrowPositionAvailable,
					topRightArrowPositionAvailable);
		}

		//allow the user to scroll through the values from left to right and vice versa
		pressedButton = buttonPressed();
		if ((pressedButton == LEFT) && (valueOffset > 20)) {
			valueOffset -= 20;
			dataChanged = true;
		} else if ((pressedButton == LEFT) && (valueOffset <= 20)) {
			valueOffset = 0;
			dataChanged = true;
		} else if ((pressedButton == RIGHT)
				&& (valueOffset < numberOfCalibrationValues - 20 - 20)) {
			valueOffset = (valueOffset + 20);
			dataChanged = true;
		} else if ((pressedButton == RIGHT)
				&& (valueOffset >= numberOfCalibrationValues - 20 - 20)) {
			valueOffset = (numberOfCalibrationValues - 20);
			dataChanged = true;
		} else {
			dataChanged = false;
		}
	}
}

//create special characters in LCD memory these contain the horizontal lines
// we use to generate a graph of our calibration data
// using simple lines instead of full bars means we can use the same characters above and below zero
// because we only have 8 special chars, we would run out if we used bars!
void createSpecialCharacter(int number) {
	byte specialCharacter[8];

	for (int i = 0; i < 8; i++) {
		specialCharacter[i] = 0b00000;
	}
	specialCharacter[number - 1] = 0b11111;
	lcd_createChar(number - 1, specialCharacter);
}

// we need 8 characters to use each line in a 5x8 pixel character cell
void makeCalibrationChars() {
	for (int i = 1; i <= 8; i++) {
		createSpecialCharacter(i);
	}
}

//dump the calibration array to the serial port for review
void doCalibrationDump() {
	setInterrupt(false);
	lcd_clear();
	lcd_setCursor(0, 1);
	lcd_print(txtConnectSerial);
	lcd_setCursor(0, 1);
	Serial.begin(getBaud(settings.baudRate));
	if (Serial) {

		Serial.println(txtSerialHeader);
		Serial.println(unitsAsText());
		for (int i = 0; i < numberOfCalibrationValues; i++) {
			Serial.print(i);
			Serial.print("  \t");
			for (uint8_t sensor = 1; sensor < (NUM_SENSORS); sensor++) {
				Serial.print(
						differenceToPreferredUnits(
								(int) ((int8_t) EEPROM.read(
										getCalibrationTableOffsetByPosition(
												sensor, i)))));
				Serial.print("  \t");
			}
			Serial.print("\n");
		}
		Serial.print(txtSerialFooter);
	}
	setInterrupt(true);
}

void serialOut(unsigned int value[]) {

	if(!Serial.availableForWrite()) Serial.begin(getBaud(settings.baudRate));
	for (uint8_t sensor = 0; sensor < (NUM_SENSORS); sensor++) {
		Serial.print(convertToPreferredUnits(value[sensor], ambientPressure));
		Serial.print("\t");
	}
	Serial.print("\n");
}

void serialWriteHeader() {
	Serial.write(START_PACKET);
}

void serialWriteInteger(unsigned int value) {
	uint16_t lowByteMask = 0b01111111;
	uint8_t bigEnd = value >> 7; //shift the upper byte down into the lower byte
	uint8_t littleEnd = value & lowByteMask; //blank out the upper byte

	Serial.write(bigEnd); //big endian in serial comms means the high byte comes first
	Serial.write(littleEnd);
}

void serialOutBytes(unsigned int value[]) {
	if(!Serial.availableForWrite()) Serial.begin(getBaud(settings.baudRate));
	serialWriteHeader();

	for (uint8_t sensor = 0; sensor < (NUM_SENSORS); sensor++) {
		serialWriteInteger(value[sensor]);
	}
}

//dump calibrated sensor data directly to serial
void doDataDump() {
	dataDumpMode = true;
	if (settings.arduinoCompatible) {
		doDataDumpChars();
	} else {
		doDataDumpBinary();
	}
	dataDumpMode = false;
}

void doDataDumpChars() {
	int reading = 0;

	lcd_clear();
	lcd_setCursor(0, 1);
	lcd_print(txtConnectSerial);
	Serial.begin(getBaud(settings.baudRate));

	if (Serial) {
		lcd_setCursor(0, 1);
		lcd_print(txtDumpingSensorData);

		Serial.println(F("0\t0\t0\t0"));

		while (!(buttonPressed() == CANCEL)) {

			for (uint8_t sensor = 0; sensor < (NUM_SENSORS); sensor++) {
				reading = readSensorCalibrated(sensor);
				Serial.print(convertToPreferredUnits(reading, ambientPressure));
				Serial.print("\t");
			}
			Serial.print("\n");
		}
		Serial.print(txtSerialFooter);
	}
}
void doDataDumpBinary() {
	unsigned long startTime = 0;
	unsigned int millisecond = 0;

	lcd_clear();
	lcd_setCursor(0, 1);
	lcd_print(txtConnectSerial);
	Serial.begin(getBaud(settings.baudRate));
	setInterrupt(true);

	if (Serial) {
		lcd_setCursor(0, 1);
		lcd_print(txtDumpingSensorData);
		startTime = millis();

		while (!(buttonPressed() == CANCEL)) {//loop while interrupt routine gathers data

			if(isSerialAllowed){
				millisecond = millis() - startTime;
				if (millisecond >= 1000) {
					startTime = millis();
					millisecond = 0;
				}

				serialWriteHeader();
				serialWriteInteger(millisecond);

				for (uint8_t sensor = 0; sensor < (NUM_SENSORS); sensor++) {
					serialWriteInteger(average[sensor]);
				}

			}
			isSerialAllowed = false;
		}

	}
	setInterrupt(false);
}

void doRevs() {
	setInterrupt(false);
	bool descending = false;
	bool previousDescending = false;

	unsigned long peak = millis();
	unsigned int delta;
	unsigned long previousPeak = millis();
	unsigned long lastUpdateTime = millis();

	int measurement = 1024;
	int previous = 1024;
	int descentCount = 0;
	int ascentCount = 0;
	int factor = settings.emaFactor;
	longAverages rpmAverage;

	initRpmDisplay();

	while (!(buttonPressed() == CANCEL)) {

		measurement = readSensorRaw(0);

		if (measurement < (previous)) {
			descentCount++;
			ascentCount--;
		}
		if (measurement > (previous)) {
			ascentCount++;
			descentCount--;
		}
		if (descentCount > 2) {
			descending = true;
			ascentCount = 0;
			descentCount = 0;
		}

		if (ascentCount > 4) {
			descending = false;
			descentCount = 0;
			ascentCount = 0;
		}

		if (previousDescending == true && descending == false) {

			peak = millis();
			delta = peak - previousPeak;
			if (delta < 1) {
				rpm = 0;
			} else {
				rpmAverage.longVal = longExponentialMovingAverage( factor, rpmAverage.longVal, 120000 / delta);
				rpm = rpmAverage.intVal[1];
			}
			previousPeak = peak;
		}

		previousDescending = descending;
		previous = measurement;

		if (millis() - peak > 2000)
			rpm = 0;

		if (millis() - lastUpdateTime > 200L) {
			updateRpmDisplay(rpm);
			lastUpdateTime = millis();
		}
	}
	lcd_clear();
	setInterrupt(true);
}

void initRpmDisplay() {
	lcd_clear();
	lcd_setCursor(6, 0);
	lcd_print(txtRpm);
	lcd_setCursor(0, 1);
	lcd_print(txtRpmScale);
}

void updateRpmDisplay(unsigned int rpm) {

	char bars[DISPLAY_COLS + 1];
	byte bar[8];
	uint8_t TotalNumberOfLitSegments = 1000000L / 10000 * rpm / 10000;
	uint8_t numberOfLitBars = TotalNumberOfLitSegments / 5;
	uint8_t numberOfLitSegments = TotalNumberOfLitSegments % 5;

	makeBars(bars, numberOfLitBars, 0);
	makeChar(bar, numberOfLitSegments);
	lcd_createChar(6, bar);

	printLcdInteger(rpm, 0, 0, 6);

	lcd_setCursor(0, 2);
	lcd_print(bars);
	if (numberOfLitBars < DISPLAY_COLS) {
		lcd_setCursor(numberOfLitBars, 2);
		lcd_write(byte(6));
	}
	lcd_setCursor(0, 3);
	lcd_print(bars);
	if (numberOfLitBars < DISPLAY_COLS) {
		lcd_setCursor(numberOfLitBars, 3);
		lcd_write(byte(6));
	}

}

void makeCenterBars(char *bars, int8_t number) {
	if (number > 10)
		number = 10;
	if (number < -10)
		number = -10;

	if (number < 0) {
		for (int8_t i = 0; i < 10 + number; i++) {
			bars[i] = ' ';
		}
		for (int8_t i = 10 + number; i < 10; i++) {
			bars[i] = 0xff;
		}
		for (int8_t i = 10; i < 20; i++) {
			bars[i] = ' ';
		}
	}

	if (number == 0) {
		for (int8_t i = 0; i <= 20; i++) {
			bars[i] = ' ';
		}
	}

	if (number > 0) {
		for (int8_t i = 0; i <= 10; i++) {
			bars[i] = ' ';
		}
		for (int8_t i = 10; i <= 10 + number; i++) {
			bars[i] = 0xff;
		}
		for (int8_t i = 10 + number; i < 20; i++) {
			bars[i] = ' ';
		}
	}

	bars[DISPLAY_COLS] = 0x00;
}

//used to detect ambient pressure for readings that ascend with vacuum in stead of going down toward absolute 0 pressure
int detectAmbient() {
	unsigned long total = 0;
	uint8_t numberOfSamples = 200;        //set the number of samples to average

	for (int i = 0; i < numberOfSamples; i++) {

		total += readSensorRaw(0);                      //add the reading we got
	}
	return total / numberOfSamples; //divide the result by the number of samples for the resulting average
}

void doMatrixDemo() {
	uint8_t colspeed[20];
	char matrix[4][20];

	while (!buttonPressed()) {
		//set the column speeds
		for (int col = 0; col < 20; col++) {
			colspeed[col] = (rand() % 7) + 1;
		}
		for (int i = 0; i < 256; i++) {
			delay(50);
			if (buttonPressed())
				return;

			for (int col = 0; col < 20; col++) {

				if (i % colspeed[col] == 0) {

					lcd_setCursor(col, 3);
					lcd_printChar(matrix[2][col]);
					matrix[3][col] = matrix[2][col];

					lcd_setCursor(col, 2);
					lcd_printChar(matrix[1][col]);
					matrix[2][col] = matrix[1][col];

					lcd_setCursor(col, 1);
					lcd_printChar(matrix[0][col]);
					matrix[1][col] = matrix[0][col];

					lcd_setCursor(col, 0);
					matrix[0][col] = rand() % 256;
					lcd_printChar(matrix[0][col]);
				}
			}
		}
	}
}

int measureLCDSpeed() {
	unsigned long microseconds = micros();
	lcd_clear();
	for (int i = 0; i < 1000; i++) {
		lcd_setCursor(0, 1);
		lcd_printChar(' ');
	}
	return micros() - microseconds;
}

void doAbsoluteDemo() {
	bool silent = settings.silent;
	settings.silent = true;
	unsigned int values[4];

	for (int i = 0; i <= 1024; i += 30) {
		for (int j = 0; j < NUM_SENSORS; j++) {
			values[j] = i;
		}
		lcdBarsSmooth(values);
	}
	for (unsigned int i = 1024; i >= 30; i -= 30) {
		for (int j = 0; j < NUM_SENSORS; j++) {
			values[j] = i;
		}
		lcdBarsSmooth(values);
	}

	settings.silent = silent;
}
void doRelativeDemo() {
	unsigned int values[4];
	bool silent = settings.silent;
	int masterValue = 500;
	int delta = 100;
	uint8_t master = settings.master;
	settings.silent = true;
	settings.master = 4;
	for (int i = masterValue + delta; i >= masterValue - delta; i -= 4) {
		values[0] = i;
		values[1] = i;
		values[2] = i;
		values[3] = masterValue;
		lcdBarsCenterSmooth(values);
	}
	settings.master = 1;
	for (int i = masterValue - delta; i <= masterValue + delta; i += 4) {
		values[0] = masterValue;
		values[1] = i;
		values[2] = i;
		values[3] = i;
		lcdBarsCenterSmooth(values);
	}
	settings.master = master;
	settings.silent = silent;
}

void doDeviceInfo() {
	unsigned long speed = measureLCDSpeed();
	lcd_setCursor(0, 0);
	lcd_print(txtVersion);
	lcd_print(F(SOFTWARE_VERSION));

	lcd_setCursor(0, 1);
	lcd_print(txtSettingsBytes);
	lcd_printInt(sizeof(settings));

	lcd_setCursor(0, 2);
	lcd_print(txtFreeRam);
	lcd_printInt(freeMemory());

	lcd_setCursor(0, 3);
	lcd_print(txtLcdSpeed);
	lcd_printLong(speed);
	waitForAnyKey();
}

ISR(TIMER1_COMPA_vect){
	unsigned long microSecond = micros();
	periodUs = microSecond - lastInterrupt;
	lastInterrupt = microSecond;

	if(dataDumpMode){
		for (int sensor = 0; sensor < settings.cylinders; sensor++) { //loop over all sensors
			average[sensor] = readSensorCalibrated(sensor);
		}
	} else {
		switch (settings.averagingMethod) {
			case 0:
				intRunningAverage();
				break;		//calculate the averages and return asap
			case 1:
				descendingAverage();
				break;
		}
	}
	interruptDurationUs = micros() - microSecond;
	isSerialAllowed = true;
}
