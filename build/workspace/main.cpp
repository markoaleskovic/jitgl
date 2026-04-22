// Available: init(), update(), renderFrame()
// EngineContext* ctx gives you: time, deltaTime, width, height, vao, vbo
// All OpenGL functions are available directly.

extern "C" void init(EngineContext* ctx) {
    // Called once after first successful JIT compile
	GLClearColor(255,255,255);
}

extern "C" void update(EngineContext* ctx) {
    // Called every frame before renderFrame
}

extern "C" void renderFrame(EngineContext* ctx) {

}










