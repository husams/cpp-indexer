struct Point {
    int x;
    int y;
};


class PointClass {
private:
    int x;
    int y;
public:
    PointClass(int x, int y) : x(x), y(y) {}
    PointClass() : x(0), y(0) {}
    int getX() const { return x; }
    int getY() const { return y; }
    void setX(int x) { this->x = x; }
    void setY(int y) { this->y = y; }
};

union PointUnion {
    int x;
    int y;
};