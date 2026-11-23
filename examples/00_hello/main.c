// 00_hello: open a window and clear it to cream.
#include "anown.h"

int main(void) {
    an_window_run(&(an_window_desc){
        .title = "anown: hello",
        .clear = AN_CREAM,
    });
    return 0;
}
