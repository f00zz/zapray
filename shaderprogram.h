#pragma once

#include <GL/glew.h>

#include <array>
#include <vector>
#include <string_view>

#include <boost/noncopyable.hpp>

#include <glm/glm.hpp>

class ShaderProgram : private boost::noncopyable
{
public:
    ShaderProgram();
    ~ShaderProgram();

    void add_shader(GLenum type, const std::string &path);
    void link();

    void bind() const;
    static void unbind();

    int uniform_location(std::string_view name) const;

    void set_uniform(int location, int v) const;
    void set_uniform(int location, float v) const;
    void set_uniform(int location, const glm::vec2 &v) const;
    void set_uniform(int location, const glm::vec3 &v) const;
    void set_uniform(int location, const glm::vec4 &v) const;

    void set_uniform(int location, const std::vector<float> &v) const;
    void set_uniform(int location, const std::vector<glm::vec2> &v) const;
    void set_uniform(int location, const std::vector<glm::vec3> &v) const;
    void set_uniform(int location, const std::vector<glm::vec4> &v) const;

    void set_uniform(int location, const glm::mat3 &mat) const;
    void set_uniform(int location, const glm::mat4 &mat) const;

private:
    GLuint id_;
};
