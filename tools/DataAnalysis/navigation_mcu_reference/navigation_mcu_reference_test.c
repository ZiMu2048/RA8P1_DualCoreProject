#include "navigation_mcu_reference.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_VECTOR_MAGIC (UINT32_C(0x4E415654))

static int read_exact(void * destination, size_t elementSize, size_t count, FILE * file)
{
    return fread(destination, elementSize, count, file) == count;
}

int main(int argc, char ** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s navigation_c_test_vectors.bin\n", argv[0]);
        return 2;
    }

    FILE * file = fopen(argv[1], "rb");
    if (file == NULL)
    {
        perror("fopen");
        return 2;
    }

    uint32_t header[6];
    if (!read_exact(header, sizeof(header[0]), 6U, file) ||
            header[0] != TEST_VECTOR_MAGIC || header[1] != 1U ||
            header[3] != NAVIGATION_ROI_WIDTH ||
            header[4] != NAVIGATION_ROI_HEIGHT ||
            header[5] != NAVIGATION_FEATURE_COUNT)
    {
        fprintf(stderr, "Invalid test-vector header.\n");
        fclose(file);
        return 2;
    }

    uint8_t image[NAVIGATION_ROI_WIDTH * NAVIGATION_ROI_HEIGHT];
    int32_t expectedFeatures[NAVIGATION_FEATURE_COUNT];
    uint32_t featureMismatchCount = 0U;
    uint32_t scoreMismatchCount = 0U;
    uint32_t decisionMismatchCount = 0U;
    int32_t maximumFeatureError = 0;

    for (uint32_t sample = 0U; sample < header[2]; sample++)
    {
        int64_t expectedScore;
        uint8_t expectedStop;
        if (!read_exact(image, sizeof(image[0]), sizeof(image), file) ||
                !read_exact(expectedFeatures, sizeof(expectedFeatures[0]),
                    NAVIGATION_FEATURE_COUNT, file) ||
                !read_exact(&expectedScore, sizeof(expectedScore), 1U, file) ||
                !read_exact(&expectedStop, sizeof(expectedStop), 1U, file))
        {
            fprintf(stderr, "Unexpected EOF at sample %" PRIu32 ".\n", sample);
            fclose(file);
            return 2;
        }

        navigation_result_t actual;
        navigation_evaluate(image, &actual);
        for (uint32_t feature = 0U; feature < NAVIGATION_FEATURE_COUNT; feature++)
        {
            int32_t error = actual.features_q[feature] - expectedFeatures[feature];
            int32_t absoluteError = error < 0 ? -error : error;
            if (absoluteError > maximumFeatureError)
            {
                maximumFeatureError = absoluteError;
            }
            if (error != 0)
            {
                if (featureMismatchCount < 8U)
                {
                    fprintf(stderr, "Feature mismatch sample=%" PRIu32
                        " feature=%" PRIu32 " expected=%" PRId32
                        " actual=%" PRId32 "\n", sample, feature,
                        expectedFeatures[feature], actual.features_q[feature]);
                }
                featureMismatchCount++;
            }
        }
        if (actual.score_q != expectedScore)
        {
            scoreMismatchCount++;
        }
        if (actual.stop_required != (expectedStop != 0U))
        {
            decisionMismatchCount++;
        }
    }

    fclose(file);
    printf("samples=%" PRIu32 " feature_mismatches=%" PRIu32
        " max_feature_error=%" PRId32 " score_mismatches=%" PRIu32
        " decision_mismatches=%" PRIu32 "\n", header[2],
        featureMismatchCount, maximumFeatureError, scoreMismatchCount,
        decisionMismatchCount);
    return (featureMismatchCount == 0U && scoreMismatchCount == 0U &&
        decisionMismatchCount == 0U) ? 0 : 1;
}
