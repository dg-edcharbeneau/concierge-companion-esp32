/* Host stub: the three cJSON calls voices_add_language_enum() makes, recording
 * the strings so the test can check the enum it would have sent. */
#pragma once
typedef struct cJSON { int unused; } cJSON;
cJSON *cJSON_AddArrayToObject(cJSON *parent, const char *key);
cJSON *cJSON_CreateString(const char *s);
int cJSON_AddItemToArray(cJSON *array, cJSON *item);
