# INV-14: queryProbe is defined and called once.


def queryProbe():
    return 0


def user():
    return queryProbe()
