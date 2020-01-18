#include "panic.h"

#include "shaderprogram.h"
#include "spritebatcher.h"
#include "pixmap.h"
#include "texture.h"
#include "geometry.h"
#include "trajectory.h"
#include "tilesheet.h"
#include "util.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <glm/vec2.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

#include <rapidjson/document.h>

#include <cassert>
#include <array>
#include <vector>
#include <algorithm>
#include <tuple>
#include <memory>
#include <iostream>

#define DRAW_ACTIVE_TRAJECTORIES
#define DRAW_COLLISIONS

std::unique_ptr<TileSheet> g_sprite_sheet;
SpriteBatcher *g_sprite_batcher;

constexpr const auto SpriteScale = 2.0f;

enum
{
    DPad_Up     = 1,
    DPad_Down   = 2,
    DPad_Left   = 4,
    DPad_Right  = 8,
};
unsigned g_dpad_state = 0;

struct Wave
{
    int start_tic;
    int spawn_interval;
    int spawn_count;
    float foe_speed;
    const Trajectory *trajectory;
};

struct Level
{
    std::vector<std::unique_ptr<Trajectory>> trajectories;
    std::vector<std::unique_ptr<Wave>> waves;
};

struct Player
{
    glm::vec2 position;
};

struct Foe
{
    Foe(const Wave *wave);
    float speed;
    const Trajectory *trajectory;
    glm::vec2 position;
    float trajectory_position;
};

Foe::Foe(const Wave *wave)
    : speed(wave->foe_speed)
    , trajectory(wave->trajectory)
    , position(trajectory->point_at(0.0f))
    , trajectory_position(0.0f)
{
}

struct Sprite
{
public:
    Sprite(const Tile *tile);

    const Tile *tile;

    bool collides_with(const Sprite &other, const glm::vec2 &pos) const;

private:
    void initialize_mask();

    std::vector<uint64_t> masks_; // for collision detection
};

Sprite::Sprite(const Tile *tile)
    : tile(tile)
{
    initialize_mask();
}

bool Sprite::collides_with(const Sprite &other, const glm::vec2 &pos) const
{
    const auto cols = tile->size.x;
    const auto rows = tile->size.y;

    const auto other_cols = other.tile->size.x;
    const auto other_rows = other.tile->size.y;

    const auto row_offset = static_cast<int>(pos.y);
    const auto col_offset = static_cast<int>(pos.x);

    if (col_offset >= cols || col_offset < -other_cols)
        return false;

    assert(masks_.size() == rows);

    for (int row = 0; row < rows; ++row)
    {
        const auto other_row = row + row_offset;
        if (other_row >= 0 && other_row < other_rows)
        {
            const auto mask = masks_[row];
            const auto other_mask = other.masks_[other_row];
            if (col_offset > 0)
            {
                if (mask & (other_mask >> col_offset))
                    return true;
            }
            else
            {
                if (mask & (other_mask << -col_offset))
                    return true;
            }
        }
    }

    return false;
}

void Sprite::initialize_mask()
{
    const auto *pm = tile->texture->pixmap();
    assert(pm->type == Pixmap::PixelType::RGBAlpha); // XXX for now

    const uint32_t *pixels = reinterpret_cast<const uint32_t *>(pm->pixels.data());

    masks_.reserve(tile->size.y);

    for (int i = 0; i < tile->size.y; ++i)
    {
        uint64_t mask = 0;
        for (int j = 0; j < tile->size.x; ++j)
        {
            uint32_t pixel = pixels[(i + tile->position.y) * pm->width + j + tile->position.x];
            mask <<= 1;
            if ((pixel >> 24) > 0x7f)
            {
                mask |= 1;
                std::cout << '*';
            }
            else
            {
                std::cout << '.';
            }
        }
        masks_.push_back(mask);
        std::cout << '\n';
    }
}

constexpr const int TicsPerSecond = 60;

class World
{
public:
    World(int window_width, int window_height);

    void initialize_level(const Level *level);
    void advance(float dt);
    void render();

private:
    void advance_one_tic();
    void advance_waves();
    void advance_foes();
    void advance_player();

    const Level *cur_level_ = nullptr;

    struct ActiveWave
    {
        ActiveWave(const Wave *wave);
        const Wave *wave;
#ifdef DRAW_ACTIVE_TRAJECTORIES
        Geometry<std::tuple<glm::vec2>> geometry;
#endif
    };

    std::vector<std::unique_ptr<ActiveWave>> active_waves_;
    std::vector<Foe> foes_;
    Player player_;
    Sprite player_sprite_; // XXX for now
    Sprite foe_sprite_; // XXX for now
    float timestamp_ = 0.0f; // milliseconds
    int cur_tic_ = 0;
#ifdef DRAW_ACTIVE_TRAJECTORIES
    ShaderProgram trajectory_program_;
#endif
};

// XXX shouldn't need to pass window_width/height here
World::World(int window_width, int window_height)
    : player_sprite_(g_sprite_sheet->find_tile("stella.png"))
    , foe_sprite_(g_sprite_sheet->find_tile("mame.png"))
{
    player_.position = glm::vec2(0.5f * window_width, 0.5f * window_height);

#ifdef DRAW_ACTIVE_TRAJECTORIES
    trajectory_program_.add_shader(GL_VERTEX_SHADER, "resources/shaders/dummy.vert");
    trajectory_program_.add_shader(GL_FRAGMENT_SHADER, "resources/shaders/dummy.frag");
    trajectory_program_.link();

    const auto projection_matrix = glm::ortho(0.0f, static_cast<float>(window_width), 0.0f, static_cast<float>(window_height));
    trajectory_program_.bind();
    trajectory_program_.set_uniform(trajectory_program_.uniform_location("mvp"), projection_matrix);
#endif
}

void World::initialize_level(const Level *level)
{
    cur_level_ = level;

    foes_.clear();
    active_waves_.clear();

    timestamp_ = 0.0f;
    int cur_tic_ = 0;

#if 0
    {
        const auto *wave = level->waves.front().get();
        Foe foe(wave);
        foe.position = {50.f, 200.f};
        foes_.push_back(foe);
    }
#endif

    advance_waves();
}

void World::advance(float dt)
{
    timestamp_ += dt;
    constexpr auto MillisecondsPerTic = 1000.0f / TicsPerSecond;

    while (timestamp_ > MillisecondsPerTic)
    {
        timestamp_ -= MillisecondsPerTic;
        ++cur_tic_;
        advance_one_tic();
    }
}

void World::render()
{
#ifdef DRAW_ACTIVE_TRAJECTORIES
    trajectory_program_.bind();
    for (const auto &wave : active_waves_)
    {
        wave->geometry.render(GL_LINE_STRIP);
    }
#endif

#ifdef DRAW_COLLISIONS
    {
        bool has_collisions = std::any_of(foes_.begin(), foes_.end(), [this](const Foe &foe) {
            const auto pos = (1.0f / SpriteScale) * (player_.position - foe.position);
            return foe_sprite_.collides_with(player_sprite_, pos);
        });
        if (has_collisions)
        {
            glClearColor(1, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
#endif

    g_sprite_batcher->start_batch();

    const auto draw_tile = [](const Tile *tile, const glm::vec2 &pos) {
        const auto half_width = 0.5f * tile->size.x * SpriteScale;
        const auto half_height = 0.5f * tile->size.y * SpriteScale;
        g_sprite_batcher->add_sprite(tile, {
                {pos + glm::vec2(-half_width, -half_height),
                 pos + glm::vec2(-half_width, half_height),
                 pos + glm::vec2(half_width, half_height),
                 pos + glm::vec2(half_width, -half_height)}},
                0);
    };

    const auto *foe_tile = foe_sprite_.tile;
    const auto *player_tile = player_sprite_.tile;

    for (const auto &foe : foes_)
    {
        draw_tile(foe_tile, foe.position);
    }

    draw_tile(player_tile, player_.position);

    g_sprite_batcher->render_batch();
}

World::ActiveWave::ActiveWave(const Wave *wave)
    : wave(wave)
{
#ifdef DRAW_ACTIVE_TRAJECTORIES
    const auto *trajectory = wave->trajectory;

    std::vector<std::tuple<glm::vec2>> verts;

    constexpr const auto NumVerts = 100;
    for (int i = 0; i < NumVerts; ++i)
    {
        const auto t = static_cast<float>(i) / (NumVerts - 1);
        const auto d = t * trajectory->length();
        const auto v = trajectory->point_at(d);
        verts.emplace_back(v);
    }

    geometry.set_data(verts);
#endif
}

void World::advance_one_tic()
{
    advance_waves();
    advance_foes();
    advance_player();
}

void World::advance_waves()
{
    for (const auto &wave : cur_level_->waves)
    {
        if (wave->start_tic == cur_tic_)
        {
            active_waves_.emplace_back(new ActiveWave(wave.get()));
        }
    }

    auto it = active_waves_.begin();
    while (it != active_waves_.end())
    {
        const auto *wave = (*it)->wave;
        bool erase = false;
        const auto wave_tic = cur_tic_ - wave->start_tic;
        if (wave_tic % wave->spawn_interval == 0)
        {
            foes_.emplace_back(wave);
            erase = (wave_tic == wave->spawn_interval * (wave->spawn_count - 1));
        }
        if (erase)
        {
            it = active_waves_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void World::advance_foes()
{
    auto it = foes_.begin();
    while (it != foes_.end())
    {
        auto &foe = *it;
        foe.trajectory_position += foe.speed;
        if (foe.trajectory_position > foe.trajectory->length())
        {
            it = foes_.erase(it);
        }
        else
        {
            foe.position = foe.trajectory->point_at(foe.trajectory_position);
            ++it;
        }
    }
}

void World::advance_player()
{
    const float speed = 2.0f;

    if (g_dpad_state & DPad_Up)
        player_.position += glm::vec2(0.f, speed);
    if (g_dpad_state & DPad_Down)
        player_.position += glm::vec2(0.f, -speed);
    if (g_dpad_state & DPad_Left)
        player_.position += glm::vec2(-speed, 0.f);
    if (g_dpad_state & DPad_Right)
        player_.position += glm::vec2(speed, 0.f);
}

// TODO: replace asserts with proper error checking and reporting (... maybe)

static glm::vec2 parse_vec2(const rapidjson::Value &value)
{
    assert(value.IsArray());
    const auto array = value.GetArray();
    assert(array.Size() == 2);
    return glm::vec2(array[0].GetDouble(), array[1].GetDouble());
}

static PathSegment parse_path_segment(const rapidjson::Value &value)
{
    assert(value.IsArray());
    const auto array = value.GetArray();
    assert(array.Size() == 4);
    return {parse_vec2(array[0]), parse_vec2(array[1]), parse_vec2(array[2]), parse_vec2(array[3])};
}

static std::unique_ptr<Trajectory> parse_trajectory(const rapidjson::Value &value)
{
    assert(value.IsArray());
    const auto array = value.GetArray();
    Path path;
    path.reserve(array.Size());
    std::transform(array.begin(), array.end(), std::back_inserter(path), [](const rapidjson::Value &value) {
        return parse_path_segment(value);
    });
    return std::make_unique<Trajectory>(path);
}

static std::unique_ptr<Level> load_level(const std::string &filename)
{
    const auto json = load_file(filename);

    rapidjson::Document document;
    rapidjson::ParseResult ok = document.Parse<rapidjson::kParseCommentsFlag>(json.data());
    assert(ok);

    auto level = std::make_unique<Level>();

    const auto trajectories = document["trajectories"].GetArray();
    for (const auto &value : trajectories)
    {
        level->trajectories.push_back(parse_trajectory(value));
    }

    const auto waves = document["waves"].GetArray();
    for (const auto &value : waves)
    {
        assert(value.IsObject());

        auto wave = std::make_unique<Wave>();
        wave->start_tic = value["start_tic"].GetInt();
        wave->spawn_interval = value["spawn_interval"].GetInt();
        wave->spawn_count = value["spawn_count"].GetInt();
        wave->foe_speed = value["foe_speed"].GetDouble();

        const auto trajectory_index = value["trajectory"].GetInt();
        assert(trajectory_index >= 0 && trajectory_index < level->trajectories.size());
        wave->trajectory = level->trajectories[trajectory_index].get();

        level->waves.push_back(std::move(wave));
    }

    return level;
}

static void update_dpad_state(GLFWwindow *window)
{
    unsigned state = 0;
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
        state |= DPad_Up;
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
        state |= DPad_Down;
    if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
        state |= DPad_Left;
    if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
        state |= DPad_Right;
    g_dpad_state = state;
}

int main()
{
    constexpr auto window_width = 400;
    constexpr auto window_height = 600;

    if (!glfwInit())
        panic("glfwInit failed\n");

    glfwSetErrorCallback([](int error, const char *description) { panic("GLFW error: %s\n", description); });

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 16);
    auto *window = glfwCreateWindow(window_width, window_height, "demo", nullptr, nullptr);
    if (!window)
        panic("glfwCreateWindow failed\n");

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewInit();

    glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode, int action, int mode) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GL_TRUE);
    });

    glViewport(0, 0, window_width, window_height);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    {
        g_sprite_sheet = load_tilesheet("resources/tilesheets/sheet.json");

        g_sprite_batcher = new SpriteBatcher;
        g_sprite_batcher->set_view_rectangle(0, window_width, 0, window_height);

        {
            auto level = load_level("resources/levels/level-0.json");

            World world(window_width, window_height);
            world.initialize_level(level.get());

            while (!glfwWindowShouldClose(window))
            {
                update_dpad_state(window);

                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);

                world.advance(1000.f / 60.f);
                world.render();

                glfwSwapBuffers(window);
                glfwPollEvents();
            }
        }

        delete g_sprite_batcher;
        g_sprite_sheet.reset();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
}
