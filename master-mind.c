/*
 * MasterMind implementation: template; see comments below on which parts need to be completed

 * Compile:
 gcc -c -o lcdBinary.o lcdBinary.c
 gcc -c -o master-mind.o master-mind.c
 gcc -o master-mind master-mind.o lcdBinary.o
 * Run:
 sudo ./master-mind

 OR use the Makefile to build
 > make all
 and run
 > make run
 and test
 > make test

 ***********************************************************************
 * The Low-level interface to LED, button, and LCD is based on:
 * wiringPi libraries by
 * Copyright (c) 2012-2013 Gordon Henderson.
 ***********************************************************************
 * See:
 *	https://projects.drogon.net/raspberry-pi/wiringpi/
 */

/* ======================================================= */
/* SECTION: includes                                       */
/* ------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

#include <unistd.h>
#include <string.h>
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

/* --------------------------------------------------------------------------- */
/* Config settings */
/* you can use CPP flags to e.g. print extra debugging messages */
/* or switch between different versions of the code e.g. digitalWrite() in Assembler */
#define DEBUG
#undef ASM_CODE

// =======================================================
// Tunables
// PINs (based on BCM numbering)
// For wiring see CW spec: https://www.macs.hw.ac.uk/~hwloidl/Courses/F28HS/F28HS_CW2_2022.pdf
// GPIO pin for green LED
#define LED 13
// GPIO pin for red LED
#define LED2 5
// GPIO pin for button
#define BUTTON 19
// =======================================================
// delay for loop iterations (mainly), in ms
// in mili-seconds: 0.2s
#define DELAY 200
// in micro-seconds: 3s
#define TIMEOUT 3000000
// =======================================================
// APP constants   ---------------------------------
// number of colours and length of the sequence
#define COLS 3
#define SEQL 3
// =======================================================

// generic constants
#ifndef TRUE
#define TRUE (1 == 1)
#define FALSE (1 == 2)
#endif

#define PAGE_SIZE (4 * 1024)
#define BLOCK_SIZE (4 * 1024)

#define INPUT 0
#define OUTPUT 1

#define LOW 0
#define HIGH 1

// =======================================================
// Wiring (see inlined initialisation routine)
#define STRB_PIN 24
#define RS_PIN 25
#define DATA0_PIN 23
#define DATA1_PIN 10
#define DATA2_PIN 27
#define DATA3_PIN 22

/* ======================================================= */
/* SECTION: constants and prototypes                       */
/* ------------------------------------------------------- */

// =======================================================
// char data for the CGRAM, i.e. defining new characters for the display
static unsigned char newChar[8] =
    {
        0b11111,
        0b10001,
        0b10001,
        0b10101,
        0b11111,
        0b10001,
        0b10001,
        0b11111,
};

/* Constants */
static const int colors = COLS;
static const int seqlen = SEQL;

static char *color_names[] = {"red", "green", "blue"};

static int *theSeq = NULL;

static int *seq1, *seq2, *cpy1, *cpy2;

/* --------------------------------------------------------------------------- */

// data structure holding data on the representation of the LCD
struct lcdDataStruct
{
  int bits, rows, cols;
  int rsPin, strbPin;
  int dataPins[8];
  int cx, cy;
};

static int lcdControl;

/* ***************************************************************************** */
/* INLINED fcts from wiringPi/devLib/lcd.c: */
// HD44780U Commands (see Fig 11, p28 of the Hitachi HD44780U datasheet)
#define LCD_CLEAR 0x01
#define LCD_HOME 0x02
#define LCD_ENTRY 0x04
#define LCD_CTRL 0x08
#define LCD_CDSHIFT 0x10
#define LCD_FUNC 0x20
#define LCD_CGRAM 0x40
#define LCD_DGRAM 0x80

// Bits in the entry register
#define LCD_ENTRY_SH 0x01
#define LCD_ENTRY_ID 0x02

// Bits in the control register
#define LCD_BLINK_CTRL 0x01
#define LCD_CURSOR_CTRL 0x02
#define LCD_DISPLAY_CTRL 0x04

// Bits in the function register
#define LCD_FUNC_F 0x04
#define LCD_FUNC_N 0x08
#define LCD_FUNC_DL 0x10

#define LCD_CDSHIFT_RL 0x04

// Mask for the bottom 64 pins which belong to the Raspberry Pi
//	The others are available for the other devices

#define PI_GPIO_MASK (0xFFFFFFC0)

static unsigned int gpiobase;
static uint32_t *gpio;

static int timed_out = 0;

/* ------------------------------------------------------- */
// misc prototypes
int failure(int fatal, const char *message, ...);
void waitForEnter(void);
int waitForButton(uint32_t *gpio, int button);

/* ======================================================= */
/* SECTION: hardware interface (LED, button, LCD display)  */
/* ------------------------------------------------------- */
/* low-level interface to the hardware */

/* send a @value@ (LOW or HIGH) on pin number @pin@; @gpio@ is the mmaped GPIO base address */
void digitalWrite(uint32_t *gpio, int pin, int value)
{
  int pin_offset = pin % 32;
  uint32_t pin_mask = 1 << pin_offset;

  uint32_t *gpio_register = gpio + (pin / 32);

  if (value)
  {
    // Set pin
    asm volatile("str %1, [%0, #0x1C]" : : "r"(gpio_register), "r"(pin_mask));
  }
  else
  {
    // Clear pin
    asm volatile("str %1, [%0, #0x28]" : : "r"(gpio_register), "r"(pin_mask));
  }
}

/* set the @mode@ of a GPIO @pin@ to INPUT or OUTPUT; @gpio@ is the mmaped GPIO base address */
void pinMode(uint32_t *gpio, int pin, int mode)
{
  uint32_t reg = *(gpio + (pin / 10));
  int offset = (pin % 10) * 3;
  uint32_t mask = 7 << offset;

  asm volatile(
      "bic %[reg], %[mask]"
      : [reg] "+r"(reg)
      : [mask] "r"(mask));

  // Set the pin to the specified mode (INPUT or OUTPUT)
  asm volatile(
      "orr %[reg], %[mode]"
      : [reg] "+r"(reg)
      : [mode] "r"(mode << offset));

  *(gpio + (pin / 10)) = reg;
}

/* send a @value@ (LOW or HIGH) on pin number @pin@; @gpio@ is the mmaped GPIO base address */
void writeLED(uint32_t *gpio, int led, int value)
{
  pinMode(gpio, led, 1);
  digitalWrite(gpio, led, value);
}

/* read a @value@ (LOW or HIGH) from pin number @pin@ (a button device); @gpio@ is the mmaped GPIO base address */
int readButton(uint32_t *gpio, int button)
{
  uint32_t *gpio_register = gpio + (button / 10);
  int pin_offset = (button % 10) * 3;

  // Set pin as input
  pinMode(gpio, button, 0);

  // Read pin state
  int result;
  asm volatile(
      "ldr r0, [%1, #52] \n"   // Load value from memory address gpio + 13 into r0
      "mov r1, #1 \n"          // Load immediate value 1 into r1
      "lsl r1, r1, %2 \n"      // Shift the value in r1 to the left by 'pin' bits
      "and r0, r0, r1 \n"      // Bitwise AND operation between r0 and r1, result stored in r0
      "mov %0, r0 \n"          // Move the result from r0 to the return variable"
      : "=r"(result)           // Output operand
      : "r"(gpio), "r"(button) // Input operands
      : "r0", "r1"             // Clobbered registers
  );

  return (result != 0);
}

/* wait for a button input on pin number @button@; @gpio@ is the mmaped GPIO base address */
int waitForButton(uint32_t *gpio, int button)
{
  // Loop until the button is pressed
  while (1)
  {
    // Read the state of the button
    int state = readButton(gpio, button);

    // Check if the button is pressed
    if (state == HIGH)
    {
      fprintf(stderr, "Button pressed\n");
      break;
    }
    // Delay for a short period before checking the button state again
    else
    {
      // state = ON;
      struct timespec sleeper, dummy;
      sleeper.tv_sec = 0;
      sleeper.tv_nsec = 100000000;
      nanosleep(&sleeper, &dummy);
      break;
    }
  }
}

/* ======================================================= */
/* SECTION: game logic                                     */
/* ------------------------------------------------------- */
/* AUX fcts of the game logic */

/* ********************************************************** */
/* COMPLETE the code for all of the functions in this SECTION */
/* Implement these as C functions in this file                */
/* ********************************************************** */

/* initialise the secret sequence; by default it should be a random sequence */
void initSeq()
{
  if (theSeq == NULL)
  {
    theSeq = (int *)malloc(SEQL * sizeof(int));
    if (theSeq == NULL)
    {
      fprintf(stderr, "Memory allocation for the Sequence failed \n");
      exit(EXIT_FAILURE);
    }
  }

  srand(time(NULL));

  for (int i = 0; i < SEQL; i++)
  {
    theSeq[i] = rand() % COLS + 1;
  }
}

/* display the sequence on the terminal window, using the format from the sample run in the spec */
void showSeq(int *seq)
{
  printf("Secret: ");
  for (int i = 0; i < SEQL; i++)
  {
    printf("%d ", seq[i]);
  }
  printf("\n");
}

#define NAN1 8
#define NAN2 9

/* counts how many entries in seq2 match entries in seq1 */
/* returns exact and approximate matches, encoded in a single value */
int countMatches(int *seq1, int *seq2)
{
  int exactMatches = 0;
  int approximateMatches = 0;

  int seq1Matched[SEQL] = {0};
  int seq2Matched[SEQL] = {0};

  // Count exact matches
  for (int i = 0; i < SEQL; i++)
  {
    if (seq1[i] == seq2[i])
    {
      exactMatches++;
      seq1Matched[i] = 1;
      seq2Matched[i] = 1;
    }
  }

  // Count approximate matches
  for (int i = 0; i < SEQL; i++)
  {
    if (!seq1Matched[i])
    {
      for (int j = 0; j < SEQL; j++)
      {
        if (!seq2Matched[j] && seq1[i] == seq2[j])
        {
          approximateMatches++;
          seq1Matched[i] = 1;
          seq2Matched[j] = 1;
          break;
        }
      }
    }
  }

  return exactMatches * 10 + approximateMatches;
}

/* show the results from calling countMatches on seq1 and seq1 */
void showMatches(int code, int *seq1, int *seq2, int lcd_format)
{
  /* Assuming the sequences are inputted */
  // int encodedMatches = countMatches(seq1, seq2); // exact in tens, approximate in ones
  // int approx = encodedMatches % 10;
  // int exact = (encodedMatches - approx) / 10;

  /* Assuming code is the output of countMatches */
  int approx = code % 10;
  int exact = (code - approx) / 10;

  printf("%d exact\n", exact);
  printf("%d approximate\n", approx);
}

/* parse an integer value as a list of digits, and put them into @seq@ */
/* needed for processing command-line with options -s or -u            */
void readSeq(int *seq, int val)
{

  if (seq == NULL)
  {
    seq = (int *)malloc(SEQL * sizeof(int));
  }

  int first = val / 100;
  int second = (val % 100) / 10;
  int third = (val % 100) % 10;

  seq[0] = first;
  seq[1] = second;
  seq[2] = third;
}

/* read a guess sequence fron stdin and store the values in arr */
/* only needed for testing the game logic, without button input */
int readNum(int max)
{
  printf("Enter %d numbers.\n", SEQL);

  int inputs[SEQL];
  int counter = 0;
  int input;
  while (counter < SEQL)
  {
    scanf("%d", &input);

    if (input > COLS || input < 1)
    {
      printf("Input range is between 1 and %d. Terminating\n", COLS);
      return 1;
    }

    inputs[counter] = input;
    counter++;
  }
  return 0;
}

/* ======================================================= */
/* SECTION: TIMER code                                     */
/* ------------------------------------------------------- */
/* TIMER code */

/* timestamps needed to implement a time-out mechanism */
static uint64_t startT, stopT;

/* you may need this function in timer_handler() below  */
/* use the libc fct gettimeofday() to implement it      */
uint64_t timeInMicroseconds()
{
  struct timeval currentTime;
  gettimeofday(&currentTime, NULL);
  uint64_t microseconds = (uint64_t)currentTime.tv_sec * 1000000 + (uint64_t)currentTime.tv_usec;

  return microseconds;
}

/* this should be the callback, triggered via an interval timer, */
/* that is set-up through a call to sigaction() in the main fct. */
void timer_handler(int signum)
{
  uint64_t time = timeInMicroseconds();
}

/* ======================================================= */
/* SECTION: Aux function                                   */
/* ------------------------------------------------------- */
/* misc aux functions */
int failure(int fatal, const char *message, ...)
{
  va_list argp;
  char buffer[1024];

  if (!fatal) //  && wiringPiReturnCodes)
    return -1;

  va_start(argp, message);
  vsnprintf(buffer, 1023, message, argp);
  va_end(argp);

  fprintf(stderr, "%s", buffer);
  exit(EXIT_FAILURE);

  return 0;
}

/*
 * waitForEnter:
 *********************************************************************************
 */

void waitForEnter(void)
{
  printf("Press ENTER to continue: ");
  (void)fgetc(stdin);
}

/*
 * delay:
 *	Wait for some number of milliseconds
 *********************************************************************************
 */
void delay(unsigned int howLong)
{
  struct timespec sleeper, dummy;

  sleeper.tv_sec = (time_t)(howLong / 1000);
  sleeper.tv_nsec = (long)(howLong % 1000) * 1000000;

  nanosleep(&sleeper, &dummy);
}

void delayMicroseconds(unsigned int howLong)
{
  struct timespec sleeper;
  unsigned int uSecs = howLong % 1000000;
  unsigned int wSecs = howLong / 1000000;

  /**/ if (howLong == 0)
    return;
#if 0
  else if (howLong  < 100)
    delayMicrosecondsHard (howLong) ;
#endif
  else
  {
    sleeper.tv_sec = wSecs;
    sleeper.tv_nsec = (long)(uSecs * 1000L);
    nanosleep(&sleeper, NULL);
  }
}

/* ======================================================= */
/* SECTION: LCD functions                                  */
/* ------------------------------------------------------- */
/* medium-level interface functions (all in C) */
/* from wiringPi:
 * strobe:
 *	Toggle the strobe (Really the "E") pin to the device.
 *	According to the docs, data is latched on the falling edge.
 *********************************************************************************
 */
void strobe(const struct lcdDataStruct *lcd)
{

  // Note timing changes for new version of delayMicroseconds ()
  digitalWrite(gpio, lcd->strbPin, 1);
  delayMicroseconds(50);
  digitalWrite(gpio, lcd->strbPin, 0);
  delayMicroseconds(50);
}

/*
 * sentDataCmd:
 *	Send an data or command byte to the display.
 *********************************************************************************
 */

void sendDataCmd(const struct lcdDataStruct *lcd, unsigned char data)
{
  register unsigned char myData = data;
  unsigned char i, d4;

  if (lcd->bits == 4)
  {
    d4 = (myData >> 4) & 0x0F;
    for (i = 0; i < 4; ++i)
    {
      digitalWrite(gpio, lcd->dataPins[i], (d4 & 1));
      d4 >>= 1;
    }
    strobe(lcd);

    d4 = myData & 0x0F;
    for (i = 0; i < 4; ++i)
    {
      digitalWrite(gpio, lcd->dataPins[i], (d4 & 1));
      d4 >>= 1;
    }
  }
  else
  {
    for (i = 0; i < 8; ++i)
    {
      digitalWrite(gpio, lcd->dataPins[i], (myData & 1));
      myData >>= 1;
    }
  }
  strobe(lcd);
}

/*
 * lcdPutCommand:
 *	Send a command byte to the display
 *********************************************************************************
 */
void lcdPutCommand(const struct lcdDataStruct *lcd, unsigned char command)
{
#ifdef DEBUG
  fprintf(stderr, "lcdPutCommand: digitalWrite(%d,%d) and sendDataCmd(%d,%d)\n", lcd->rsPin, 0, lcd, command);
#endif
  digitalWrite(gpio, lcd->rsPin, 0);
  sendDataCmd(lcd, command);
  delay(2);
}

void lcdPut4Command(const struct lcdDataStruct *lcd, unsigned char command)
{
  register unsigned char myCommand = command;
  register unsigned char i;

  digitalWrite(gpio, lcd->rsPin, 0);

  for (i = 0; i < 4; ++i)
  {
    digitalWrite(gpio, lcd->dataPins[i], (myCommand & 1));
    myCommand >>= 1;
  }
  strobe(lcd);
}

/*
 * lcdHome: lcdClear:
 *	Home the cursor or clear the screen.
 *********************************************************************************
 */
void lcdHome(struct lcdDataStruct *lcd)
{
#ifdef DEBUG
  fprintf(stderr, "lcdHome: lcdPutCommand(%d,%d)\n", lcd, LCD_HOME);
#endif
  lcdPutCommand(lcd, LCD_HOME);
  lcd->cx = lcd->cy = 0;
  delay(5);
}

void lcdClear(struct lcdDataStruct *lcd)
{
#ifdef DEBUG
  fprintf(stderr, "lcdClear: lcdPutCommand(%d,%d) and lcdPutCommand(%d,%d)\n", lcd, LCD_CLEAR, lcd, LCD_HOME);
#endif
  lcdPutCommand(lcd, LCD_CLEAR);
  lcdPutCommand(lcd, LCD_HOME);
  lcd->cx = lcd->cy = 0;
  delay(5);
}

/*
 * lcdPosition:
 *	Update the position of the cursor on the display.
 *	Ignore invalid locations.
 *********************************************************************************
 */
void lcdPosition(struct lcdDataStruct *lcd, int x, int y)
{
  // struct lcdDataStruct *lcd = lcds [fd] ;

  if ((x > lcd->cols) || (x < 0))
    return;
  if ((y > lcd->rows) || (y < 0))
    return;

  lcdPutCommand(lcd, x + (LCD_DGRAM | (y > 0 ? 0x40 : 0x00) /* rowOff [y] */));

  lcd->cx = x;
  lcd->cy = y;
}

/*
 * lcdDisplay: lcdCursor: lcdCursorBlink:
 *	Turn the display, cursor, cursor blinking on/off
 *********************************************************************************
 */
void lcdDisplay(struct lcdDataStruct *lcd, int state)
{
  if (state)
    lcdControl |= LCD_DISPLAY_CTRL;
  else
    lcdControl &= ~LCD_DISPLAY_CTRL;

  lcdPutCommand(lcd, LCD_CTRL | lcdControl);
}

void lcdCursor(struct lcdDataStruct *lcd, int state)
{
  if (state)
    lcdControl |= LCD_CURSOR_CTRL;
  else
    lcdControl &= ~LCD_CURSOR_CTRL;

  lcdPutCommand(lcd, LCD_CTRL | lcdControl);
}

void lcdCursorBlink(struct lcdDataStruct *lcd, int state)
{
  if (state)
    lcdControl |= LCD_BLINK_CTRL;
  else
    lcdControl &= ~LCD_BLINK_CTRL;

  lcdPutCommand(lcd, LCD_CTRL | lcdControl);
}

/*
 * lcdPutchar:
 *	Send a data byte to be displayed on the display. We implement a very
 *	simple terminal here - with line wrapping, but no scrolling. Yet.
 *********************************************************************************
 */
void lcdPutchar(struct lcdDataStruct *lcd, unsigned char data)
{
  digitalWrite(gpio, lcd->rsPin, 1);
  sendDataCmd(lcd, data);

  if (++lcd->cx == lcd->cols)
  {
    lcd->cx = 0;
    if (++lcd->cy == lcd->rows)
      lcd->cy = 0;

    // TODO: inline computation of address and eliminate rowOff
    lcdPutCommand(lcd, lcd->cx + (LCD_DGRAM | (lcd->cy > 0 ? 0x40 : 0x00) /* rowOff [lcd->cy] */));
  }
}

/*
 * lcdPuts:
 *	Send a string to be displayed on the display
 *********************************************************************************
 */
void lcdPuts(struct lcdDataStruct *lcd, const char *string)
{
  while (*string)
    lcdPutchar(lcd, *string++);
}

/* ======================================================= */
/* SECTION: aux functions for game logic                   */
/* ------------------------------------------------------- */
/* interface on top of the low-level pin I/O code */

/* blink the led on pin @led@, @c@ times */
void blinkN(uint32_t *gpio, int led, int c)
{
  for (int i = 0; i < c; i++)
  {
    digitalWrite(gpio, led, HIGH);
    usleep(DELAY * 1000);

    digitalWrite(gpio, led, LOW);
    usleep(DELAY * 1000);
  }
}

/* ======================================================= */
/* SECTION: main fct                                       */
/* ------------------------------------------------------- */
int main(int argc, char *argv[])
{
  struct lcdDataStruct *lcd;
  int bits, rows, cols;
  unsigned char func;

  int found = 0, attempts = 0, i, j, code;
  int c, d, buttonPressed, rel, foo;
  int *attSeq;

  int pinLED = LED, pin2LED2 = LED2, pinButton = BUTTON;
  int fSel, shift, pin, clrOff, setOff, off, res;
  int fd;

  int exact, contained;
  char str1[32];
  char str2[32];

  struct timeval t1, t2;
  int t;

  char buf[32];

  // variables for command-line processing
  char str_in[20], str[20] = "some text";
  int verbose = 0, debug = 0, help = 0, opt_m = 0, opt_n = 0, opt_s = 0, unit_test = 0, res_matches = 0;

  // -------------------------------------------------------
  // process command-line arguments
  // see: man 3 getopt for docu and an example of command line parsing
  {
    int opt;
    while ((opt = getopt(argc, argv, "hvdus:")) != -1)
    {
      switch (opt)
      {
      case 'v':
        verbose = 1;
        break;
      case 'h':
        help = 1;
        break;
      case 'd':
        debug = 1;
        break;
      case 'u':
        unit_test = 1;
        break;
      case 's':
        opt_s = atoi(optarg);
        break;
      default: /* '?' */
        fprintf(stderr, "Usage: %s [-h] [-v] [-d] [-u <seq1> <seq2>] [-s <secret seq>]  \n", argv[0]);
        exit(EXIT_FAILURE);
      }
    }
  }

  if (help)
  {
    fprintf(stderr, "MasterMind program, running on a Raspberry Pi, with connected LED, button and LCD display\n");
    fprintf(stderr, "Use the button for input of numbers. The LCD display will show the matches with the secret sequence.\n");
    fprintf(stderr, "For full specification of the program see: https://www.macs.hw.ac.uk/~hwloidl/Courses/F28HS/F28HS_CW2_2022.pdf\n");
    fprintf(stderr, "Usage: %s [-h] [-v] [-d] [-u <seq1> <seq2>] [-s <secret seq>]  \n", argv[0]);
    exit(EXIT_SUCCESS);
  }

  if (unit_test && optind >= argc - 1)
  {
    fprintf(stderr, "Expected 2 arguments after option -u\n");
    exit(EXIT_FAILURE);
  }

  if (verbose && unit_test)
  {
    printf("1st argument = %s\n", argv[optind]);
    printf("2nd argument = %s\n", argv[optind + 1]);
  }

  if (verbose)
  {
    fprintf(stdout, "Settings for running the program\n");
    fprintf(stdout, "Verbose is %s\n", (verbose ? "ON" : "OFF"));
    fprintf(stdout, "Debug is %s\n", (debug ? "ON" : "OFF"));
    fprintf(stdout, "Unittest is %s\n", (unit_test ? "ON" : "OFF"));
    if (opt_s)
      fprintf(stdout, "Secret sequence set to %d\n", opt_s);
  }

  seq1 = (int *)malloc(seqlen * sizeof(int));
  seq2 = (int *)malloc(seqlen * sizeof(int));
  cpy1 = (int *)malloc(seqlen * sizeof(int));
  cpy2 = (int *)malloc(seqlen * sizeof(int));

  // check for -u option, and if so run a unit test on the matching function
  if (unit_test && argc > optind + 1)
  { // more arguments to process; only needed with -u
    strcpy(str_in, argv[optind]);
    opt_m = atoi(str_in);
    strcpy(str_in, argv[optind + 1]);
    opt_n = atoi(str_in);
    // CALL a test-matches function; see testm.c for an example implementation
    readSeq(seq1, opt_m); // turn the integer number into a sequence of numbers
    readSeq(seq2, opt_n); // turn the integer number into a sequence of numbers
    if (verbose)
      fprintf(stdout, "Testing matches function with sequences %d and %d\n", opt_m, opt_n);
    res_matches = countMatches(seq1, seq2);
    showMatches(res_matches, seq1, seq2, 1);
    exit(EXIT_SUCCESS);
  }
  else
  {
    /* nothing to do here; just continue with the rest of the main fct */
  }

  if (opt_s)
  { // if -s option is given, use the sequence as secret sequence
    if (theSeq == NULL)
      theSeq = (int *)malloc(seqlen * sizeof(int));
    readSeq(theSeq, opt_s);
    if (verbose)
    {
      fprintf(stderr, "Running program with secret sequence:\n");
      showSeq(theSeq);
    }
  }

  // -------------------------------------------------------
  // LCD constants, hard-coded: 16x2 display, using a 4-bit connection
  bits = 4;
  cols = 16;
  rows = 2;
  // -------------------------------------------------------

  printf("Raspberry Pi LCD driver, for a %dx%d display (%d-bit wiring) \n", cols, rows, bits);

  if (geteuid() != 0)
    fprintf(stderr, "setup: Must be root. (Did you forget sudo?)\n");

  // init of guess sequence, and copies (for use in countMatches)
  attSeq = (int *)malloc(seqlen * sizeof(int));
  cpy1 = (int *)malloc(seqlen * sizeof(int));
  cpy2 = (int *)malloc(seqlen * sizeof(int));

  // -----------------------------------------------------------------------------
  // constants for RPi3
  gpiobase = 0x3F200000;

  // -----------------------------------------------------------------------------
  // memory mapping
  // Open the master /dev/memory device
  if ((fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC)) < 0)
    return failure(FALSE, "setup: Unable to open /dev/mem: %s\n", strerror(errno));

  // GPIO:
  gpio = (uint32_t *)mmap(0, BLOCK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, gpiobase);
  if ((int32_t)gpio == -1)
    return failure(FALSE, "setup: mmap (GPIO) failed: %s\n", strerror(errno));

  // -------------------------------------------------------
  // Configuration of LED, BUTTON and LCD pins
  pinMode(gpio, pinLED, OUTPUT);
  pinMode(gpio, pin2LED2, OUTPUT);
  pinMode(gpio, pinButton, INPUT);
  pinMode(gpio, STRB_PIN, OUTPUT);
  pinMode(gpio, RS_PIN, OUTPUT);
  pinMode(gpio, DATA0_PIN, OUTPUT);
  pinMode(gpio, DATA1_PIN, OUTPUT);
  pinMode(gpio, DATA2_PIN, OUTPUT);
  pinMode(gpio, DATA3_PIN, OUTPUT);

  // -------------------------------------------------------
  // INLINED version of lcdInit (can only deal with one LCD attached to the RPi):
  // you can use this code as-is, but you need to implement digitalWrite() and
  // pinMode() which are called from this code
  // Create a new LCD:
  lcd = (struct lcdDataStruct *)malloc(sizeof(struct lcdDataStruct));
  if (lcd == NULL)
    return -1;

  // hard-wired GPIO pins
  lcd->rsPin = RS_PIN;
  lcd->strbPin = STRB_PIN;
  lcd->bits = 4;
  lcd->rows = rows; // # of rows on the display
  lcd->cols = cols; // # of cols on the display
  lcd->cx = 0;      // x-pos of cursor
  lcd->cy = 0;      // y-pos of curosr

  lcd->dataPins[0] = DATA0_PIN;
  lcd->dataPins[1] = DATA1_PIN;
  lcd->dataPins[2] = DATA2_PIN;
  lcd->dataPins[3] = DATA3_PIN;
  // lcd->dataPins [4] = d4 ;
  // lcd->dataPins [5] = d5 ;
  // lcd->dataPins [6] = d6 ;
  // lcd->dataPins [7] = d7 ;

  // lcds [lcdFd] = lcd ;

  digitalWrite(gpio, lcd->rsPin, 0);
  pinMode(gpio, lcd->rsPin, OUTPUT);
  digitalWrite(gpio, lcd->strbPin, 0);
  pinMode(gpio, lcd->strbPin, OUTPUT);

  for (i = 0; i < bits; ++i)
  {
    digitalWrite(gpio, lcd->dataPins[i], 0);
    pinMode(gpio, lcd->dataPins[i], OUTPUT);
  }
  delay(35); // mS

  if (bits == 4)
  {
    func = LCD_FUNC | LCD_FUNC_DL; // Set 8-bit mode 3 times
    lcdPut4Command(lcd, func >> 4);
    delay(35);
    lcdPut4Command(lcd, func >> 4);
    delay(35);
    lcdPut4Command(lcd, func >> 4);
    delay(35);
    func = LCD_FUNC; // 4th set: 4-bit mode
    lcdPut4Command(lcd, func >> 4);
    delay(35);
    lcd->bits = 4;
  }
  else
  {
    failure(TRUE, "setup: only 4-bit connection supported\n");
    func = LCD_FUNC | LCD_FUNC_DL;
    lcdPutCommand(lcd, func);
    delay(35);
    lcdPutCommand(lcd, func);
    delay(35);
    lcdPutCommand(lcd, func);
    delay(35);
  }

  if (lcd->rows > 1)
  {
    func |= LCD_FUNC_N;
    lcdPutCommand(lcd, func);
    delay(35);
  }

  // Rest of the initialisation sequence
  lcdDisplay(lcd, TRUE);
  lcdCursor(lcd, FALSE);
  lcdCursorBlink(lcd, FALSE);
  lcdClear(lcd);

  lcdPutCommand(lcd, LCD_ENTRY | LCD_ENTRY_ID);     // set entry mode to increment address counter after write
  lcdPutCommand(lcd, LCD_CDSHIFT | LCD_CDSHIFT_RL); // set display shift to right-to-left

  // END lcdInit ------
  // -----------------------------------------------------------------------------
  // Start of game
  fprintf(stderr, "Printing welcome message on the LCD display ...\n");

  /*-------------------------------------------------------------------------------------*/
  lcdPuts(lcd, "Welcome to");
  lcdPosition(lcd, 1, 1);
  lcdPuts(lcd, "MasterMind");
  delay(2000);
  lcdClear(lcd);

  /*-------------------------------------------------------------------------------------*/

  /* initialise the secret sequence */
  if (!opt_s)
    initSeq();
  if (debug)
    showSeq(theSeq);

  // optionally one of these 2 calls:
  lcdPuts(lcd, "Press enter");
  lcdPosition(lcd, 0, 1);
  lcdPuts(lcd, "to start");
  waitForEnter();

  // -----------------------------------------------------------------------------
  // +++++ main loop
  // Turn LED off if was ON previous game
  digitalWrite(gpio, pinLED, LOW);
  digitalWrite(gpio, pin2LED2, LOW);

  // Main game loop starts here, the player has 5 attempts to guess the secret sequence
  while (!found && attempts < 5)
  {
    int turn = 0;

    // clear the lcd from previous round
    lcdClear(lcd);

    // print the round number on the terminal
    printf("Round: %d\n", attempts += 1);

    // Print the number of attempts on the next line
    char roundString[32];
    lcdPuts(lcd, "Starting");
    lcdPosition(lcd, 0, 1);
    sprintf(roundString, "Round: %d", attempts);
    lcdPuts(lcd, roundString);

    delay(2000);

    // main loop for each turn inputting the sequence
    while (1)
    {
      printf("Turn: %d\n", turn += 1);
      printf("Enter a sequence of %d numbers\n", SEQL);
      lcdClear(lcd);

      lcdPuts(lcd, "Press the button");
      lcdPosition(lcd, 0, 1);
      lcdPuts(lcd, "now");

      // Wait for 2 seconds
      delay(1000);

      // Clear the LCD screen
      lcdClear(lcd);

      // Time window of 5 seconds
      time_t startTime = time(NULL);
      time_t endTime = startTime + 5;

      // Count of button presses
      int buttonPressCount = 0;

      // Blink red when time window ends
      while (time(NULL) < endTime)
      {
        // Wait for the button to be pressed
        if (waitForButton(gpio, pinButton) == 1)
        {
          buttonPressCount++;
          delay(500);
          lcdPuts(lcd, "Button Pressed");
          delay(300);
          lcdClear(lcd);
        }
        if (buttonPressCount >= 3)
        {
          buttonPressCount = 3;
          break;
        }
      }

      // Print the number of button presses
      printf("Button pressed %d times\n", buttonPressCount);

      // Set redLED blink for 2 seconds to indicate the end of the time window
      digitalWrite(gpio, pin2LED2, HIGH);
      delay(2000);
      digitalWrite(gpio, pin2LED2, LOW);

      // Blink the number of times the button was pressed on green
      blinkN(gpio, pinLED, buttonPressCount);

      // Store the number of button presses in attSeq
      attSeq[turn - 1] = buttonPressCount;
      // Repeat for a sequence of 3
      if (turn <= 3)
      {
        // Delay before starting the next attempt
        delay(500);
      }
      if (turn == 3)
      {
        // blink red LED twice to indicate the end of the attempt
        blinkN(gpio, pin2LED2, 2);
        break;
      }
    }

    // Compare the sequence with the secret sequence
    int matches = countMatches(attSeq, theSeq);
    int approx = matches % 10;
    int exact = (matches - approx) / 10;

    printf("%d exact \n", exact);
    printf("%d approximate \n", approx);

    delay(500);

    // prints exact on the lcd
    lcdClear(lcd);
    blinkN(gpio, pinLED, exact);
    sprintf(buf, "%d exact", exact);
    lcdPosition(lcd, 1, 0);
    lcdPuts(lcd, buf);

    // separator
    blinkN(gpio, pin2LED2, 1);

    // prints approximate on the lcd
    blinkN(gpio, pinLED, approx);
    sprintf(buf, "%d approximate", approx);
    lcdPosition(lcd, 1, 1);
    lcdPuts(lcd, buf);

    delay(1000);

    lcdClear(lcd);

    if (exact == 3)
    {
      found = 1;
      break;
    }
    else
    {
      // Clear the sequence
      for (int i = 0; i < SEQL; i++)
      {
        attSeq[i] = 0;
      }
    }
    blinkN(gpio, pin2LED2, 3);

    delay(500);
    printf("Starting next round\n");
    delay(2000);
  }

  if (found)
  {
    printf("SUCCESS\n");
    lcdPuts(lcd, "SUCCESS");

    // Wait for a short delay
    usleep(500000);

    // Print the number of attempts on the next line
    lcdPosition(lcd, 0, 1);
    char attemptsString[32];
    sprintf(attemptsString, "Attempts: %d", attempts);
    lcdPuts(lcd, attemptsString);

    // Blink green LED three times
    digitalWrite(gpio, pin2LED2, HIGH);
    blinkN(gpio, pinLED, 3);

    // Delay before clearing LCD
    usleep(500000);
    lcdClear(lcd);

    lcdPuts(lcd, "Ending game");
    usleep(1000000);

    // Clear LCD
    lcdClear(lcd);
    writeLED(gpio, pin2LED2, 0);
  }
  else
  {
    lcdClear(lcd);
    fprintf(stdout, "Sequence not found\n");
    lcdPuts(lcd, "YOU LOSE!");

    // Delay before clearing LCD
    delay(5000);
    lcdClear(lcd);

    lcdPuts(lcd, "Ending game");
    usleep(1000000);

    // Clear LCD
    lcdClear(lcd);
    writeLED(gpio, pin2LED2, 0);
  }

  // Free memory
  free(theSeq);
  free(seq1);
  free(seq2);
  free(cpy1);
  free(cpy2);
  free(attSeq);
  free(lcd);

  return 0;
}