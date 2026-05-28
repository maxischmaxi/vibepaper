// Offline unit tests for the imagegen scheme request-builders and response-
// parsers. Includes imagegen.c directly to reach its static functions; no
// network and no daemon. Build/run under ASan+UBSan — see tests in the verify
// script. Negative cases intentionally print "[bg:err]" lines to stderr.

#include "../src/imagegen.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    // --- base64 round-trip (incl. high bytes and a non-3-multiple length) ---
    {
        const uint8_t in[] = {0,1,2,3,4,250,251,252,253,254,255,42,7};
        char *b = b64_encode(in, sizeof in);
        assert(b);
        uint8_t *out = NULL; size_t n = 0;
        assert(b64_decode(b, &out, &n) == 0);
        assert(n == sizeof in && memcmp(in, out, n) == 0);
        free(b); free(out);
        printf("ok: base64 round-trip\n");
    }

    // --- build_openai_body ---
    {
        bg_gen_opts o = { .scheme=BG_SCHEME_OPENAI, .model="gpt-image-2", .size="1536x1024", .quality="high" };
        char *b = build_openai_body(&o, "a cat");
        assert(b);
        assert(strstr(b, "\"model\":\"gpt-image-2\""));
        assert(strstr(b, "\"prompt\":\"a cat\""));
        assert(strstr(b, "\"size\":\"1536x1024\""));
        assert(strstr(b, "\"quality\":\"high\""));
        assert(strstr(b, "\"n\":1"));
        free(b);
        bg_gen_opts o2 = { .scheme=BG_SCHEME_OPENAI, .model="m" };   // size/quality NULL
        char *b2 = build_openai_body(&o2, "x");
        assert(b2 && !strstr(b2, "\"size\"") && !strstr(b2, "\"quality\""));
        free(b2);
        printf("ok: build_openai_body\n");
    }

    // --- openai_parse: b64 path ("YWJj" == "abc") ---
    {
        bg_gen_result r = {0};
        assert(openai_parse("{\"data\":[{\"b64_json\":\"YWJj\"}]}", &r));
        assert(r.len == 3 && memcmp(r.data, "abc", 3) == 0 && strcmp(r.ext, "png") == 0);
        bg_imagegen_free_result(&r);
        bg_gen_result r2 = {0};
        assert(!openai_parse("{\"data\":[{}]}", &r2));   // neither b64 nor url
        assert(!openai_parse("{\"data\":[]}", &r2));     // empty data
        printf("ok: openai_parse\n");
    }

    // --- build_gemini_body (no source) ---
    {
        bg_gen_opts o = { .scheme=BG_SCHEME_GEMINI, .model="gemini-2.5-flash-image", .size="2K" };
        char *b = build_gemini_body(&o, "a dog", NULL);
        assert(b);
        assert(strstr(b, "\"contents\"") && strstr(b, "\"parts\""));
        assert(strstr(b, "\"text\":\"a dog\""));
        assert(strstr(b, "\"imageSize\":\"2K\""));
        free(b);
        bg_gen_opts o2 = { .scheme=BG_SCHEME_GEMINI, .model="m", .size="1536x1024" };
        char *b2 = build_gemini_body(&o2, "x", NULL);
        assert(b2 && !strstr(b2, "imageSize") && !strstr(b2, "generationConfig"));
        free(b2);
        printf("ok: build_gemini_body\n");
    }

    // --- gemini_parse: finds inlineData past a text part; ext from mimeType ---
    {
        const char *json =
            "{\"candidates\":[{\"content\":{\"parts\":["
            "{\"text\":\"hi\"},"
            "{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"YWJj\"}}]}}]}";
        bg_gen_result r = {0};
        assert(gemini_parse(json, &r));
        assert(r.len == 3 && memcmp(r.data, "abc", 3) == 0 && strcmp(r.ext, "jpeg") == 0);
        bg_imagegen_free_result(&r);
        bg_gen_result r2 = {0};
        assert(!gemini_parse("{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"x\"}]}}]}", &r2));
        printf("ok: gemini_parse\n");
    }

    // --- looks_like_aspect ---
    {
        assert(looks_like_aspect("16:9") && looks_like_aspect("1:1") && looks_like_aspect("21:9"));
        assert(!looks_like_aspect("1536x1024") && !looks_like_aspect("auto") && !looks_like_aspect(":9") && !looks_like_aspect("3:"));
        printf("ok: looks_like_aspect\n");
    }

    printf("ALL IMAGEGEN TESTS PASSED\n");
    return 0;
}
