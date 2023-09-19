#include <SFML/Window/Mouse.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("[Window] sf::Mouse")
{
    SECTION("isButtonPressed()")
    {
        CHECK(!sf::Mouse::isButtonPressed(sf::Mouse::Left));
        CHECK(!sf::Mouse::isButtonPressed(sf::Mouse::Right));
        CHECK(!sf::Mouse::isButtonPressed(sf::Mouse::Middle));
        CHECK(!sf::Mouse::isButtonPressed(sf::Mouse::XButton1));
        CHECK(!sf::Mouse::isButtonPressed(sf::Mouse::XButton2));
    }
}
