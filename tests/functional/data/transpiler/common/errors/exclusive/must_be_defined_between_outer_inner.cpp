@kernel void test_kern() {
    @outer for (int i = 0; i < 10; ++i) {
        @inner for (int j = 0; j < 10; ++j) {
            @exclusive int kk = 0;
            kk = i + j;
        }
    }
}
