#include <stdio.h>

int test_failures = 0;
int test_checks = 0;

void test_master(void);
void test_gate(void);
void test_algorithm(void);
void test_patch(void);
void test_state(void);
void test_session(void);
void test_envelope(void);
void test_breath(void);
void test_pair(void);
void test_bank(void);
void test_clicks(void);
void test_chamber(void);
void test_melody(void);
void test_reverb(void);
void test_tape(void);
void test_chandas(void);
void test_presets_hostile(void);
void test_pitch(void);
void test_console(void);
void test_mod(void);

int main(void) {
    struct { const char *name; void (*fn)(void); } suites[] = {
        {"master", test_master},       {"gate", test_gate},
        {"algorithm", test_algorithm}, {"patch", test_patch},
        {"state", test_state},         {"session", test_session},
        {"envelope", test_envelope},   {"breath", test_breath},
        {"pair", test_pair},           {"bank", test_bank},
        {"clicks", test_clicks},
        {"chamber", test_chamber},
        {"melody", test_melody},       {"reverb", test_reverb},
        {"tape", test_tape},           {"chandas", test_chandas},
        {"presets hostile", test_presets_hostile},
        {"pitch", test_pitch},
        {"console", test_console},
        {"mod", test_mod},
    };
    for (size_t i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        int before = test_failures;
        printf("== %s\n", suites[i].name);
        fflush(stdout);
        suites[i].fn();
        if (test_failures > before)
            printf("   %d failure(s)\n", test_failures - before);
    }
    printf("%d checks, %d failures\n", test_checks, test_failures);
    return test_failures ? 1 : 0;
}
